#include "imageproducts.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>
#include <QCoreApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QImageReader>

namespace nadirstation::products {

namespace {
std::atomic<bool> g_medianFilterEnabled{true};
}
void setMedianFilterEnabled(bool enabled) { g_medianFilterEnabled.store(enabled); }
bool medianFilterEnabled() {
    return g_medianFilterEnabled.load() && !qEnvironmentVariableIsSet("NADIR_NO_MEDIAN");
}
namespace {
inline int gray(const QImage& im, int x, int y) { return qGray(im.pixel(x, y)); }

std::pair<int, int> robustRange(const QImage& image, const QImage* paired = nullptr,
                                const QImage* validMask = nullptr, const QImage* pairedMask = nullptr,
                                double lowQuantile = 0.02, double highQuantile = 0.98) {
    std::array<quint64, 256> histogram{};
    quint64 count = 0;
    for (int y = 0; y < image.height(); ++y) {
        const uchar* row = image.constScanLine(y);
        const uchar* pairedRow = paired ? paired->constScanLine(y) : nullptr;
        const uchar* maskRow = validMask ? validMask->constScanLine(y) : nullptr;
        const uchar* pairedMaskRow = pairedMask ? pairedMask->constScanLine(y) : nullptr;
        for (int x = 0; x < image.width(); ++x) {
            const int v = row[x];
            if ((maskRow && maskRow[x] == 0) || v == 0 || v >= 250 ||
                (pairedRow && (pairedRow[x] == 0 || pairedRow[x] >= 250 ||
                               (pairedMaskRow && pairedMaskRow[x] == 0)))) continue;
            ++histogram[static_cast<std::size_t>(v)];
            ++count;
        }
    }
    if (count < 32) return {0, 255};
    auto percentile = [&](double q) {
        const quint64 target = static_cast<quint64>(q * static_cast<double>(count - 1));
        quint64 sum = 0;
        for (int i = 0; i < 256; ++i) {
            sum += histogram[static_cast<std::size_t>(i)];
            if (sum > target) return i;
        }
        return 255;
    };
    const int low = percentile(lowQuantile);
    const int high = std::max(low + 1, percentile(highQuantile));
    return {low, high};
}

int stretch8(int value, const std::pair<int, int>& range) {
    return qBound(0, qRound((value - range.first) * 255.0 / (range.second - range.first)), 255);
}

constexpr std::array<double, 64> nadirStationVisibleReflectance{{
    0.0000,0.4700,0.9424,1.4124,1.8824,2.3548,2.8248,3.2948,
    3.7672,4.2372,4.7072,5.1772,5.6496,6.1196,6.5896,7.0621,
    7.7683,8.7083,9.6507,10.5931,11.7693,13.1817,14.5941,16.0065,
    17.4189,18.8313,20.2413,21.6537,23.3024,25.1848,27.0672,28.9520,
    30.8344,32.7168,34.5992,36.4841,38.6027,40.9551,43.3099,45.6623,
    48.0171,50.3695,52.7244,55.0792,57.6678,60.4926,63.3174,66.1398,
    68.9647,71.7895,74.6143,77.4391,80.4977,83.7950,87.0898,90.3846,
    93.6794,96.9743,100.2715,103.5663,107.0973,110.8622,114.6294,118.3942
}};

int visibleDn(const QImage& image, int x, int y) {
    if (image.format() == QImage::Format_Grayscale16) {
        const auto* row = reinterpret_cast<const quint16*>(image.constScanLine(y));
        return qBound(0, static_cast<int>(row[x]), 63);
    }
    // Legacy decoder export was floor(DN * 255 / 63); rounding recovers DN.
    return qBound(0, qRound(qGray(image.pixel(x, y)) * 63.0 / 255.0), 63);
}

void fillSmallProjectedGaps(QImage& image) {
    if (image.isNull() || image.format() != QImage::Format_RGB32) return;
    constexpr QRgb background = qRgb(4, 12, 24);
    constexpr int maximumGapPixels = 64;
    const int width = image.width(), height = image.height();
    std::vector<uint8_t> visited(static_cast<std::size_t>(width) * height, 0);
    std::vector<int> component;
    std::vector<int> border;
    std::vector<int> pending;
    component.reserve(maximumGapPixels + 1);
    border.reserve(512);
    pending.reserve(maximumGapPixels + 1);
    for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            const int seed = y * width + x;
            if (visited[static_cast<std::size_t>(seed)] || row[x] != background) continue;
            component.clear();
            border.clear();
            pending.clear();
            pending.push_back(seed);
            visited[static_cast<std::size_t>(seed)] = 1;
            for (std::size_t head = 0; head < pending.size(); ++head) {
                const int index = pending[head];
                component.push_back(index);
                const int px = index % width, py = index / width;
                for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    const int nx = px + dx, ny = py + dy;
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                    const int neighborIndex = ny * width + nx;
                    const QRgb neighbor = reinterpret_cast<const QRgb*>(image.constScanLine(ny))[nx];
                    if (neighbor == background) {
                        auto& mark = visited[static_cast<std::size_t>(neighborIndex)];
                        if (!mark) { mark = 1; pending.push_back(neighborIndex); }
                    } else {
                        border.push_back(qGray(neighbor));
                    }
                }
            }
            if (component.size() > maximumGapPixels || border.size() < 4) continue;
            const std::size_t middle = border.size() / 2;
            std::nth_element(border.begin(), border.begin() + static_cast<std::ptrdiff_t>(middle), border.end());
            const int ink = border[middle];
            for (const int index : component) {
                auto* dst = reinterpret_cast<QRgb*>(image.scanLine(index / width));
                dst[index % width] = qRgb(ink, ink, std::min(255, ink + 4));
            }
        }
    }
}

QRgb reflectancePalette(double pct) {
    const double t = std::clamp(pct / 100.0, 0.0, 1.0);
    if (t < 0.25) {
        const double u = t / 0.25;
        return qRgb(qRound(10 + 15*u), qRound(20 + 90*u), qRound(75 + 130*u));
    }
    if (t < 0.60) {
        const double u = (t - 0.25) / 0.35;
        return qRgb(qRound(25 + 220*u), qRound(110 + 125*u), qRound(205 - 95*u));
    }
    const double u = (t - 0.60) / 0.40;
    return qRgb(245, qRound(235 + 20*u), qRound(110 + 145*u));
}
}

static void rebuildSegment(QImage& img, int y, int x0, int x1, int top, int bot, float t) {
    if (img.format() != QImage::Format_Grayscale8 || y < 0 || top < 0 || bot < 0 ||
        y >= img.height() || top >= img.height() || bot >= img.height() || img.width() < 8) return;
    const int width = img.width();
    x0 = std::max(0, x0); x1 = std::min(width, x1);
    if (x1 - x0 < 1) return;
    const uchar* a = img.constScanLine(top);
    const uchar* b = img.constScanLine(bot);
    auto clampX = [width](int x) { return x < 0 ? 0 : (x >= width ? width - 1 : x); };
    // grain of the rows beside the segment, measured on a stretch a little wider than it
    auto grainOf = [&](const uchar* r) {
        const int lo = std::max(1, x0 - 32), hi = std::min(width - 1, x1 + 32);
        std::vector<int> v;
        for (int x = lo; x < hi; x += 2) v.push_back(std::abs(2 * r[x] - r[x - 1] - r[std::min(width - 1, x + 1)]));
        if (v.empty()) return 0.0f;
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
        return static_cast<float>(v[v.size() / 2]) / 1.65f;
    };
    const float sigma = std::min(3.0f, 0.7f * 0.5f * (grainOf(a) + grainOf(b)));
    const int span = x1 - x0 + 2;
    std::vector<int> shift(static_cast<std::size_t>(span), 0);
    // Matching structure along the row only works while the rows it joins are close: across a long gap
    // the two sides show different things and the match turns into vertical streaks. Four scan lines
    // is the limit (a scan line is one row of IR, four of visible).
    const int matchLimit = width > 4000 ? 16 : 4;
    const bool matching = (bot - top - 1) <= matchLimit;
    for (int i = 0; i < span && matching; ++i) {
        const int x = x0 - 1 + i;
        auto cost = [&](int dx) {
            int c = 0;
            for (int k = -2; k <= 2; ++k) c += std::abs(a[clampX(x + dx + k)] - b[clampX(x - dx + k)]);
            return c;
        };
        int best = cost(0), bestDx = 0;
        for (int dx = -4; dx <= 4; ++dx) {
            if (dx == 0) continue;
            const int c = cost(dx);
            if (c + 12 < best) { best = c; bestDx = dx; }
        }
        shift[static_cast<std::size_t>(i)] = bestDx;
    }
    uint32_t seed = 2166136261u ^ static_cast<uint32_t>(y * 16777619u) ^ static_cast<uint32_t>(top * 40503u) ^ static_cast<uint32_t>(x0 * 69069u);
    auto grain = [&seed]() {
        float g = 0.0f;
        for (int i = 0; i < 4; ++i) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            g += static_cast<float>(seed & 0xFFFF) / 65535.0f - 0.5f;
        }
        return g * 1.7320508f;
    };
    // Across a long gap the two sides share little; interpolating them pixel by pixel draws vertical
    // streaks, and a plain blur of them gives a flat band. So the gap is filled in two parts: the low
    // frequencies are interpolated between the two sides (smoothed more towards the middle of the gap),
    // and the texture -- each row minus its own short blur -- is mirrored from the rows just outside,
    // the upper rows' texture in the upper half, the lower rows' in the lower half. Texture the
    // surroundings really have, and no vertical structure.
    std::vector<float> lowA, lowB, bigA, bigB, texT, texB;
    int smoothLo = 0;
    const int gapRows = bot - top - 1;
    if (!matching) {
        const int smallRadius = 6;
        const int bigRadius = std::max(8, std::min(80, gapRows * 3));
        const int lo = std::max(0, x0 - bigRadius), hi = std::min(width, x1 + bigRadius);
        auto blur = [&](const uchar* r, int radius, std::vector<float>& out) {
            out.assign(static_cast<std::size_t>(hi - lo), 0.0f);
            std::vector<long> prefix(static_cast<std::size_t>(hi - lo) + 1, 0);
            for (int x = lo; x < hi; ++x) prefix[static_cast<std::size_t>(x - lo + 1)] = prefix[static_cast<std::size_t>(x - lo)] + r[x];
            for (int x = lo; x < hi; ++x) {
                const int p = std::max(lo, x - radius), q = std::min(hi, x + radius + 1);
                out[static_cast<std::size_t>(x - lo)] = static_cast<float>(prefix[static_cast<std::size_t>(q - lo)] - prefix[static_cast<std::size_t>(p - lo)]) / static_cast<float>(q - p);
            }
        };
        blur(a, smallRadius, lowA);
        blur(b, smallRadius, lowB);
        blur(a, bigRadius, bigA);
        blur(b, bigRadius, bigB);
        smoothLo = lo;
        // texture rows: mirror of the rows outside the gap, nearest the gap first
        const int mirrorY = std::max(0, std::min(img.height() - 1, top - (y - top - 1)));
        const int mirrorB = std::max(0, std::min(img.height() - 1, bot + (bot - y - 1)));
        std::vector<float> lowMT, lowMB;
        blur(img.constScanLine(mirrorY), smallRadius, lowMT);
        blur(img.constScanLine(mirrorB), smallRadius, lowMB);
        texT.assign(lowMT.size(), 0.0f);
        texB.assign(lowMB.size(), 0.0f);
        const uchar* rt = img.constScanLine(mirrorY);
        const uchar* rb = img.constScanLine(mirrorB);
        for (std::size_t k = 0; k < texT.size(); ++k) {
            texT[k] = static_cast<float>(rt[smoothLo + static_cast<int>(k)]) - lowMT[k];
            texB[k] = static_cast<float>(rb[smoothLo + static_cast<int>(k)]) - lowMB[k];
        }
    }
    uchar* dst = img.scanLine(y);
    for (int x = x0; x < x1; ++x) {
        const int i = x - x0 + 1;
        if (!matching) {
            const float w = 1.0f - std::pow(std::fabs(1.0f - 2.0f * t), 3.0f);   // 0 at the edges, 1 mid-gap
            const std::size_t k = static_cast<std::size_t>(x - smoothLo);
            const float av = (1.0f - w) * lowA[k] + w * bigA[k];
            const float bv = (1.0f - w) * lowB[k] + w * bigB[k];
            const float wt = std::clamp((t - 0.4f) / 0.2f, 0.0f, 1.0f);
            const float texture = (1.0f - wt) * texT[k] + wt * texB[k];
            const float v = (1.0f - t) * av + t * bv + texture;
            dst[x] = static_cast<uchar>(qBound(0, static_cast<int>(std::lround(v)), 255));
            continue;
        }
        int v5[5];
        for (int q = -2; q <= 2; ++q) v5[q + 2] = shift[static_cast<std::size_t>(std::min(span - 1, std::max(0, i + q)))];
        std::sort(v5, v5 + 5);
        const int dx = v5[2];
        const float v = (1.0f - t) * a[clampX(x + dx)] + t * b[clampX(x - dx)] + sigma * grain();
        dst[x] = static_cast<uchar>(qBound(0, static_cast<int>(std::lround(v)), 255));
    }
}

void rebuildRowEdgeAware(QImage& img, int y, int top, int bot, float t) {
    rebuildSegment(img, y, 0, img.width(), top, bot, t);
}

QImage repairMissingLines(const QImage& input, int maxGap, int* repairedRows, bool detectNoisyRows, int* noisyRows) {
    if (repairedRows) *repairedRows = 0;
    if (noisyRows) *noisyRows = 0;
    const bool grayscale = input.format() == QImage::Format_Grayscale8;
    const QImage source = grayscale ? input : input.convertToFormat(QImage::Format_RGB32);
    const QImage graySource = input.convertToFormat(QImage::Format_Grayscale8);
    QImage out = source.copy();
    if (out.height() < 3 || graySource.width() < 1) return out;
    QVector<int> suspectRows(out.height(), 0);
    const int stride = std::max(1, graySource.width() / 512);
    const int sampleCount = (graySource.width() + stride - 1) / stride;
    for (int y = 0; y < out.height(); ++y) {
        int zeros = 0;
        const uchar* row = graySource.constScanLine(y);
        for (int x = 0; x < graySource.width(); x += stride) zeros += row[x] == 0;
        if (zeros * 100 >= sampleCount * 98) {
            suspectRows[y] = 1;
            continue;
        }
        if (y == 0 || y == out.height() - 1) continue;

        // Also catch a damaged scan line that is not blank but has a coherent
        // row-wide offset. Require the two adjacent rows to agree closely,
        // then require a large median residual and a strong same-sign majority.
        const uchar* above = graySource.constScanLine(y - 1);
        const uchar* below = graySource.constScanLine(y + 1);
        int adjacentAgreement = 0;
        int positiveOutliers = 0;
        int negativeOutliers = 0;
        std::vector<int> absoluteResiduals;
        absoluteResiduals.reserve(static_cast<std::size_t>(sampleCount));
        for (int x = 0; x < graySource.width(); x += stride) {
            const int a = above[x];
            const int b = below[x];
            if (std::abs(a - b) <= 8) ++adjacentAgreement;
            const int residual = static_cast<int>(row[x]) - (a + b) / 2;
            if (residual > 12) ++positiveOutliers;
            else if (residual < -12) ++negativeOutliers;
            absoluteResiduals.push_back(std::abs(residual));
        }
        const auto middle = absoluteResiduals.begin() + absoluteResiduals.size() / 2;
        std::nth_element(absoluteResiduals.begin(), middle, absoluteResiduals.end());
        const int medianResidual = *middle;
        const int sameSignOutliers = std::max(positiveOutliers, negativeOutliers);
        if (adjacentAgreement * 100 >= sampleCount * 90 && medianResidual >= 18 &&
            sameSignOutliers * 100 >= sampleCount * 75) {
            suspectRows[y] = 1;
            continue;
        }

        // A one-row stripe can contaminate the immediate neighbors used by
        // the radius-1 check. Compare against rows two scans away in that case;
        // the lower thresholds are paired with stricter support on both sides.
        if (y >= 2 && y + 2 < out.height()) {
            const uchar* above2 = graySource.constScanLine(y - 2);
            const uchar* below2 = graySource.constScanLine(y + 2);
            int outerAgreement = 0;
            int positive = 0;
            int negative = 0;
            std::vector<int> signedResiduals;
            std::vector<int> absoluteResiduals;
            signedResiduals.reserve(static_cast<std::size_t>(sampleCount));
            absoluteResiduals.reserve(static_cast<std::size_t>(sampleCount));
            for (int x = 0; x < graySource.width(); x += stride) {
                const int a = above2[x];
                const int b = below2[x];
                if (std::abs(a - b) <= 8) ++outerAgreement;
                const int residual = static_cast<int>(row[x]) - (a + b) / 2;
                if (residual > 10) ++positive;
                else if (residual < -10) ++negative;
                signedResiduals.push_back(residual);
                absoluteResiduals.push_back(std::abs(residual));
            }
            auto signedMiddle = signedResiduals.begin() + signedResiduals.size() / 2;
            auto absoluteMiddle = absoluteResiduals.begin() + absoluteResiduals.size() / 2;
            std::nth_element(signedResiduals.begin(), signedMiddle, signedResiduals.end());
            std::nth_element(absoluteResiduals.begin(), absoluteMiddle, absoluteResiduals.end());
            const int medianSignedResidual = *signedMiddle;
            const int medianAbsoluteResidual = *absoluteMiddle;
            const int sameSign = std::max(positive, negative);
            if (outerAgreement * 100 >= sampleCount * 85 && medianAbsoluteResidual >= 10 &&
                std::abs(medianSignedResidual) >= 6 && sameSign * 100 >= sampleCount * 35) {
                suspectRows[y] = 1;
            }
        }
    }
    // Noisy rows. A scan line whose sector failed badly arrives as salt-and-pepper: the high bits of
    // many pixels are wrong, so a pixel jumps far from both of its neighbours in the row and the jump
    // has the same sign on either side (a spike). Real imagery has few of those at this threshold.
    // The row is judged against the rows around it, so a bright textured scene is not mistaken for
    // noise, and against an absolute ceiling for the case where most rows are bad. A noisy row is
    // then treated exactly like a blank one: cut out and rebuilt from the good rows beside it.
    if (detectNoisyRows && out.height() >= 20 && graySource.width() >= 64) {
        const int w = graySource.width(), h = graySource.height();
        constexpr int kSpike = 20;
        std::vector<float> spike(static_cast<std::size_t>(h), 0.0f);
        for (int y = 0; y < h; ++y) {
            const uchar* row = graySource.constScanLine(y);
            int count = 0;
            for (int x = 1; x + 1 < w; ++x) {
                const int d1 = row[x] - row[x - 1], d2 = row[x] - row[x + 1];
                if ((d1 > kSpike && d2 > kSpike) || (d1 < -kSpike && d2 < -kSpike)) ++count;
            }
            spike[static_cast<std::size_t>(y)] = static_cast<float>(count) / static_cast<float>(w - 2);
        }
        std::vector<float> window;
        int flagged = 0;
        for (int y = 1; y + 1 < h; ++y) {
            if (suspectRows[y]) continue;
            window.clear();
            for (int k = -8; k <= 8; ++k) {
                const int yy = y + k;
                if (k == 0 || yy < 0 || yy >= h || suspectRows[yy]) continue;
                window.push_back(spike[static_cast<std::size_t>(yy)]);
            }
            if (window.size() < 4) continue;
            std::nth_element(window.begin(), window.begin() + window.size() / 2, window.end());
            const float base = window[window.size() / 2];
            const float here = spike[static_cast<std::size_t>(y)];
            if ((here >= 0.04f && here >= 2.5f * base + 0.02f) || here >= 0.15f) {
                suspectRows[y] = 2;   // 2 = judged noisy, 1 = blank or offset
                ++flagged;
            }
        }
        // Rows that are bright or dark as a whole against both sides. A scan line that arrives with a
        // wrong high bit in most pixels has no spikes (the pixels are consistently off), but its mean
        // stands out from the rows on either side, which agree with one another; an edge in the scene
        // stands out from one side only.
        std::vector<float> rowMean(static_cast<std::size_t>(h), 0.0f);
        const int step = std::max(1, w / 1024);
        for (int y = 0; y < h; ++y) {
            const uchar* row = graySource.constScanLine(y);
            long sum = 0; int n = 0;
            for (int x = 0; x < w; x += step) { sum += row[x]; ++n; }
            rowMean[static_cast<std::size_t>(y)] = static_cast<float>(sum) / static_cast<float>(std::max(1, n));
        }
        auto sideMedian = [&](int y, int dir, float& out) {
            float v[6]; int n = 0;
            for (int k = 1; k <= 32 && n < 6; ++k) {
                const int yy = y + dir * k;
                if (yy < 0 || yy >= h) break;
                if (suspectRows[yy]) continue;
                v[n++] = rowMean[static_cast<std::size_t>(yy)];
            }
            if (n < 3) return false;
            std::sort(v, v + n);
            out = v[n / 2];
            return true;
        };
        std::vector<int> extra;
        for (int y = 1; y + 1 < h; ++y) {
            if (suspectRows[y]) continue;
            float up = 0, dn = 0;
            if (!sideMedian(y, -1, up) || !sideMedian(y, +1, dn)) continue;
            if (std::fabs(up - dn) > 12.0f) continue;     // the two sides disagree: a real edge
            const float here = rowMean[static_cast<std::size_t>(y)];
            const float side = 0.5f * (up + dn);
            if (std::fabs(here - side) >= std::max(18.0f, 0.3f * side)) extra.push_back(y);
        }
        for (int y : extra) { suspectRows[y] = 2; ++flagged; }
        if (noisyRows) *noisyRows = flagged;
    }

    // Rebuilding a row. Plain linear blending between the rows above and below turns a lost scan line
    // into a soft smear: it is smoother than anything the instrument produces and edges across it
    // double. Two things make it read like the rest of the picture. Each pixel looks along the row for
    // the displacement at which the row above and the row below agree best, and takes its value from
    // those two matching places, so an edge that crosses the gap stays one edge. And the grey
    // channels get back the fine grain of the rows beside them, measured there, which a blend loses.
    const int width = out.width();
    auto clampX = [width](int x) { return x < 0 ? 0 : (x >= width ? width - 1 : x); };
    auto grainOf = [&](const uchar* r) {
        std::vector<int> v;
        v.reserve(static_cast<std::size_t>(width / 4 + 1));
        for (int x = 1; x + 1 < width; x += 4) v.push_back(std::abs(2 * r[x] - r[x - 1] - r[x + 1]));
        if (v.empty()) return 0.0f;
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
        return static_cast<float>(v[v.size() / 2]) / 1.65f;   // median |2x - a - b| of Gaussian grain is 1.65 sigma
    };
    for (int y = 1; y < out.height() - 1;) {
        if (!suspectRows[y]) { ++y; continue; }
        // The whole run of suspect rows. It used to be cut at maxGap and interpolated towards the
        // next row, which is itself suspect (usually blank), so a run longer than maxGap became a
        // ramp fading to black -- the dark stripes on low-SNR FY-2 images. A run that is too long
        // to bridge is now left as it is.
        int y2 = y;
        while (y2 + 1 < out.height() && suspectRows[y2 + 1]) ++y2;
        const int top = y - 1;
        const int bot = y2 + 1;
        // A burst of bad scan lines is bridged up to three times maxGap when it has good rows on both
        // sides. Grain or a black stripe shown as image is worse than a plausible fill, and a burst
        // that long is still short against the structure it crosses. Longer runs (the blank space
        // beyond the limb, a real outage) stay as they are.
        const int reach = detectNoisyRows ? 3 * maxGap : maxGap;
        // Beyond about four scan lines the rows on the two sides no longer show the same structure and
        // matching them draws vertical streaks; rebuildSegment fills such a gap from interpolated low
        // frequencies and mirrored texture instead.
        const int longGap = width > 4000 ? 16 : 4;
        if (grayscale && y2 - y + 1 <= reach && y2 - y + 1 > longGap && top >= 0 && bot < out.height()) {
            for (int yy = y; yy <= y2; ++yy)
                rebuildSegment(out, yy, 0, width, top, bot, static_cast<float>(yy - top) / static_cast<float>(bot - top));
            if (repairedRows) *repairedRows += y2 - y + 1;
        } else if (y2 - y + 1 <= reach && top >= 0 && bot < out.height()) {
            const uchar* gTop = graySource.constScanLine(top);
            const uchar* gBot = graySource.constScanLine(bot);
            const float sigma = grayscale
                ? std::min(3.0f, 0.7f * 0.5f * (grainOf(gTop) + grainOf(gBot))) : 0.0f;
            std::vector<int> shift(static_cast<std::size_t>(width), 0);
            for (int x = 0; x < width; ++x) {
                auto cost = [&](int dx) {
                    int c = 0;
                    for (int k = -2; k <= 2; ++k) c += std::abs(gTop[clampX(x + dx + k)] - gBot[clampX(x - dx + k)]);
                    return c;
                };
                int best = cost(0), bestDx = 0;
                for (int dx = -4; dx <= 4; ++dx) {
                    if (dx == 0) continue;
                    const int c = cost(dx);
                    if (c + 12 < best) { best = c; bestDx = dx; }
                }
                shift[static_cast<std::size_t>(x)] = bestDx;
            }
            // Smooth the displacement field: a lone odd match in a flat area is noise, not structure.
            std::vector<int> settled(shift);
            for (int x = 1; x + 1 < width; ++x) {
                const int a = shift[static_cast<std::size_t>(x - 1)], b = shift[static_cast<std::size_t>(x)], c = shift[static_cast<std::size_t>(x + 1)];
                settled[static_cast<std::size_t>(x)] = std::max(std::min(a, b), std::min(std::max(a, b), c));   // median of three
            }
            uint32_t seed = 2166136261u ^ static_cast<uint32_t>(y * 16777619u);
            auto grain = [&seed]() {
                // four uniforms summed: close enough to Gaussian for a few grey levels of grain
                float g = 0.0f;
                for (int i = 0; i < 4; ++i) {
                    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                    g += static_cast<float>(seed & 0xFFFF) / 65535.0f - 0.5f;
                }
                return g * 1.7320508f;   // unit variance
            };
            for (int yy = y; yy <= y2; ++yy) {
                const float t = static_cast<float>(yy - top) / static_cast<float>(bot - top);
                for (int x = 0; x < width; ++x) {
                    const int dx = settled[static_cast<std::size_t>(x)];
                    const int xa = clampX(x + dx), xb = clampX(x - dx);
                    if (grayscale) {
                        const float v = (1.0f - t) * source.constScanLine(top)[xa] + t * source.constScanLine(bot)[xb]
                                      + sigma * grain();
                        out.scanLine(yy)[x] = static_cast<uchar>(qBound(0, static_cast<int>(std::lround(v)), 255));
                    } else {
                        const QRgb a = reinterpret_cast<const QRgb*>(source.constScanLine(top))[xa];
                        const QRgb b = reinterpret_cast<const QRgb*>(source.constScanLine(bot))[xb];
                        const int r = qRound((1.0f-t)*qRed(a)+t*qRed(b));
                        const int g = qRound((1.0f-t)*qGreen(a)+t*qGreen(b));
                        const int bl = qRound((1.0f-t)*qBlue(a)+t*qBlue(b));
                        reinterpret_cast<QRgb*>(out.scanLine(yy))[x] = qRgb(qBound(0,r,255),qBound(0,g,255),qBound(0,bl,255));
                    }
                }
            }
            if (repairedRows) *repairedRows += y2 - y + 1;
        }
        y = y2 + 1;
    }
    // Partly damaged rows. A fade often ruins only a stretch of a scan line, so the row as a whole looks
    // fine and escapes the row test while leaving a short streak: white or black dashes, or a patch
    // of grain. The same judgement is made per 32-pixel block against the same block in the rows
    // around it, and flagged blocks are rebuilt from the good blocks above and below.
    if (detectNoisyRows && grayscale && out.height() >= 20 && out.width() >= 128) {
        constexpr int B = 32;
        const int w = out.width(), h = out.height();
        const int nb = w / B;
        std::vector<float> sp(static_cast<std::size_t>(h) * nb, 0.0f), zr(static_cast<std::size_t>(h) * nb, 0.0f);
        constexpr int kSpike = 20;
        for (int y = 0; y < h; ++y) {
            const uchar* row = out.constScanLine(y);
            for (int k = 0; k < nb; ++k) {
                const int x0 = k * B, x1 = x0 + B;
                int spikes = 0, zeros = 0;
                for (int x = x0; x < x1; ++x) {
                    zeros += row[x] == 0;
                    if (x == 0 || x + 1 >= w) continue;
                    const int d1 = row[x] - row[x - 1], d2 = row[x] - row[x + 1];
                    if ((d1 > kSpike && d2 > kSpike) || (d1 < -kSpike && d2 < -kSpike)) ++spikes;
                }
                sp[static_cast<std::size_t>(y) * nb + k] = static_cast<float>(spikes) / B;
                zr[static_cast<std::size_t>(y) * nb + k] = static_cast<float>(zeros) / B;
            }
        }
        std::vector<uint8_t> bad(static_cast<std::size_t>(h) * nb, 0);
        std::vector<float> wa, wb;
        for (int y = 1; y + 1 < h; ++y) {
            for (int k = 0; k < nb; ++k) {
                wa.clear(); wb.clear();
                for (int d = -6; d <= 6; ++d) {
                    const int yy = y + d;
                    if (d == 0 || yy < 0 || yy >= h) continue;
                    wa.push_back(sp[static_cast<std::size_t>(yy) * nb + k]);
                    wb.push_back(zr[static_cast<std::size_t>(yy) * nb + k]);
                }
                std::nth_element(wa.begin(), wa.begin() + wa.size() / 2, wa.end());
                std::nth_element(wb.begin(), wb.begin() + wb.size() / 2, wb.end());
                const float baseS = wa[wa.size() / 2], baseZ = wb[wb.size() / 2];
                const float s = sp[static_cast<std::size_t>(y) * nb + k], z = zr[static_cast<std::size_t>(y) * nb + k];
                bool flagged = (s >= 0.15f && s >= 3.0f * baseS + 0.06f) || (z >= 0.9f && baseZ < 0.3f);
                if (!flagged) {
                    // A dash of black or white, or a patch that is simply wrong, has no spikes inside
                    // it. It shows against the rows directly above and below, which agree with each
                    // other there: most of the block sits far from their mean.
                    const uchar* rm = out.constScanLine(y - 1);
                    const uchar* r0 = out.constScanLine(y);
                    const uchar* rp = out.constScanLine(y + 1);
                    int far = 0, agree = 0;
                    for (int x = k * B; x < (k + 1) * B; ++x) {
                        far += std::abs(r0[x] - (rm[x] + rp[x]) / 2) > 35;
                        agree += std::abs(rm[x] - rp[x]) <= 14;
                    }
                    flagged = far >= (B * 4) / 10 && agree >= (B * 8) / 10;
                }
                if (flagged) bad[static_cast<std::size_t>(y) * nb + k] = 1;
            }
        }
        // a damaged stretch does not end exactly on a block edge: take in a neighbour that is also off
        std::vector<uint8_t> grown(bad);
        for (int y = 1; y + 1 < h; ++y)
            for (int k = 0; k < nb; ++k) {
                if (bad[static_cast<std::size_t>(y) * nb + k]) continue;
                const bool nearBad = (k > 0 && bad[static_cast<std::size_t>(y) * nb + k - 1]) ||
                                     (k + 1 < nb && bad[static_cast<std::size_t>(y) * nb + k + 1]);
                if (nearBad && sp[static_cast<std::size_t>(y) * nb + k] >= 0.06f) grown[static_cast<std::size_t>(y) * nb + k] = 1;
            }
        bad.swap(grown);
        const int reachRows = 3 * maxGap;
        int blocksFixed = 0;
        const QImage frozen = out;   // fill from the rows as they were, not from half-rebuilt ones
        for (int k = 0; k < nb; ++k) {
            for (int y = 1; y + 1 < h;) {
                if (!bad[static_cast<std::size_t>(y) * nb + k]) { ++y; continue; }
                int y2 = y;
                while (y2 + 1 < h && bad[static_cast<std::size_t>(y2 + 1) * nb + k]) ++y2;
                const int top = y - 1, bot = y2 + 1;
                if (y2 - y + 1 <= reachRows && top >= 0 && bot < h) {
                    for (int yy = y; yy <= y2; ++yy) {
                        const float t = static_cast<float>(yy - top) / static_cast<float>(bot - top);
                        // rebuildSegment reads `top` and `bot` from the image it writes to; they are
                        // good blocks and are not touched, so out can be used directly
                        rebuildSegment(out, yy, k * B, (k + 1) * B, top, bot, t);
                        ++blocksFixed;
                    }
                }
                y = y2 + 1;
            }
        }
        Q_UNUSED(frozen);
        if (noisyRows) *noisyRows += blocksFixed / std::max(1, nb);   // rows' worth of blocks
    }
    return out;
}

QImage repairMissingLinesMedian(const QImage& input, int maxGap, int* repairedRows,
                                int gray16ValidMinimum, int gray16ValidMaximum) {
    if (repairedRows) *repairedRows = 0;
    const QImage::Format format = input.format();
    if (input.isNull() || input.height() < 3 || maxGap < 1 ||
        (format != QImage::Format_Grayscale16 && format != QImage::Format_Grayscale8 &&
         format != QImage::Format_Indexed8)) return input;
    auto sample = [&](int x, int y) -> int {
        if (format == QImage::Format_Grayscale16)
            return reinterpret_cast<const quint16*>(input.constScanLine(y))[x];
        return input.constScanLine(y)[x];
    };
    auto valid = [&](int value) {
        if (format == QImage::Format_Grayscale16)
            return value >= gray16ValidMinimum && value <= gray16ValidMaximum;
        if (format == QImage::Format_Indexed8) return value != 255;
        return value <= 100; // cloud-fraction raster: 0-100 valid, 255 invalid
    };
    QVector<int> validCounts(input.height(), 0);
    const int stride = std::max(1, input.width() / 1024);
    const int sampleCount = (input.width() + stride - 1) / stride;
    for (int y = 0; y < input.height(); ++y)
        for (int x = 0; x < input.width(); x += stride)
            validCounts[y] += valid(sample(x, y));

    QVector<int> suspect(input.height(), 0);
    for (int y = 1; y + 1 < input.height(); ++y) {
        const int current = validCounts[y];
        const int above = validCounts[y - 1], below = validCounts[y + 1];
        if (current * 100 <= sampleCount * 3 &&
            above * 100 >= sampleCount * 25 && below * 100 >= sampleCount * 25)
            suspect[y] = 1;

        // Also catch a short, fully populated scanline that is a strong
        // outlier against both neighbours. A bad line can contain plausible
        // numeric values, so nodata-count checks alone miss it. Use a strict
        // threshold plus neighbour agreement to avoid smoothing real cloud
        // edges and gradual scene changes.
        QVector<int> residuals;
        QVector<int> neighbourDeltas;
        residuals.reserve(sampleCount);
        neighbourDeltas.reserve(sampleCount);
        int common = 0, categoryMismatchAbove = 0, categoryMismatchBelow = 0,
            categoryNeighbourMismatch = 0;
        for (int x = 0; x < input.width(); x += stride) {
            const int a = sample(x, y - 1), c = sample(x, y), b = sample(x, y + 1);
            if (!valid(a) || !valid(c) || !valid(b)) continue;
            ++common;
            if (format == QImage::Format_Grayscale16) {
                residuals.append(qAbs(2 * c - a - b) / 2);
                neighbourDeltas.append(qAbs(a - b));
            } else {
                categoryMismatchAbove += (a != c);
                categoryMismatchBelow += (b != c);
                categoryNeighbourMismatch += (a != b);
            }
        }
        if (common * 100 >= sampleCount * 35) {
            if (format == QImage::Format_Grayscale16) {
                auto medianOf = [](QVector<int>& values) {
                    const auto middle = values.begin() + values.size() / 2;
                    std::nth_element(values.begin(), middle, values.end());
                    return *middle;
                };
                if (!residuals.isEmpty() && !neighbourDeltas.isEmpty()) {
                    const int rowResidual = medianOf(residuals);
                    const int naturalChange = medianOf(neighbourDeltas);
                    if (rowResidual >= 800 && rowResidual > naturalChange * 3 + 400)
                        suspect[y] = 1;
                }
            } else {
                const double mismatchUp = static_cast<double>(categoryMismatchAbove) / common;
                const double mismatchDown = static_cast<double>(categoryMismatchBelow) / common;
                const double neighbourMismatch = static_cast<double>(categoryNeighbourMismatch) / common;
                if (mismatchUp >= 0.60 && mismatchDown >= 0.60 && neighbourMismatch <= 0.12)
                    suspect[y] = 1;
            }
        }
    }

    QImage out = input.copy();
    int repaired = 0;
    for (int y = 1; y + 1 < input.height();) {
        if (!suspect[y]) { ++y; continue; }
        int end = y;
        while (end + 1 < input.height() - 1 && suspect[end + 1]) ++end;
        const int top = y - 1, bottom = end + 1;
        const int gapLength = end - y + 1;
        if (gapLength <= maxGap && validCounts[top] * 100 >= sampleCount * 25 &&
            validCounts[bottom] * 100 >= sampleCount * 25) {
            for (int row = y; row <= end; ++row) {
                auto* dst8 = format == QImage::Format_Grayscale16 ? nullptr : out.scanLine(row);
                auto* dst16 = format == QImage::Format_Grayscale16
                    ? reinterpret_cast<quint16*>(out.scanLine(row)) : nullptr;
                for (int x = 0; x < input.width(); ++x) {
                    const int a = sample(x, top), b = sample(x, bottom);
                    const bool va = valid(a), vb = valid(b);
                    if (!va && !vb) continue;
                    const double fraction = static_cast<double>(row - top) / (bottom - top);
                    int value = va && vb ? qRound(a + (b - a) * fraction) : (va ? a : b);
                    if (format == QImage::Format_Indexed8 && va && vb) {
                        // Indexed cloud classes must remain valid codes; use
                        // the local mode from the two adjacent scanlines.
                        int candidates[6], count = 0;
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int sx = std::clamp(x + dx, 0, input.width() - 1);
                            for (const int sy : {top, bottom}) {
                                const int candidate = sample(sx, sy);
                                if (valid(candidate)) candidates[count++] = candidate;
                            }
                        }
                        int bestCount = -1;
                        for (int i = 0; i < count; ++i) {
                            int votes = 0;
                            for (int j = 0; j < count; ++j) votes += candidates[j] == candidates[i];
                            if (votes > bestCount) { bestCount = votes; value = candidates[i]; }
                        }
                    }
                    if (dst16) dst16[x] = static_cast<quint16>(value);
                    else dst8[x] = static_cast<uchar>(value);
                }
                ++repaired;
            }
        }
        y = end + 1;
    }
    // Geostationary-to-map projection bends a missing sensor scanline into a
    // short, slanted run of no-data pixels. A whole-row detector cannot see
    // that pattern, so also repair only short vertical holes bounded by valid
    // pixels. Edge-of-image no-data and longer missing areas are untouched.
    auto sampleOut = [&](int x, int y) -> int {
        if (format == QImage::Format_Grayscale16)
            return reinterpret_cast<const quint16*>(out.constScanLine(y))[x];
        return out.constScanLine(y)[x];
    };
    for (int x = 0; x < out.width(); ++x) {
        for (int y = 0; y < out.height();) {
            if (valid(sampleOut(x, y))) { ++y; continue; }
            const int start = y;
            while (y < out.height() && !valid(sampleOut(x, y))) ++y;
            const int end = y - 1;
            const int gapLength = end - start + 1;
            if (start == 0 || y >= out.height() || gapLength > maxGap) continue;
            const int above = sampleOut(x, start - 1);
            const int below = sampleOut(x, y);
            if (!valid(above) || !valid(below)) continue;
            for (int row = start; row <= end; ++row) {
                const double fraction = static_cast<double>(row - start + 1) / (gapLength + 1);
                int value = qRound(above + (below - above) * fraction);
                if (format == QImage::Format_Indexed8) {
                    int candidates[6], count = 0;
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int sx = std::clamp(x + dx, 0, out.width() - 1);
                        for (const int sy : {start - 1, y}) {
                            const int candidate = sampleOut(sx, sy);
                            if (valid(candidate)) candidates[count++] = candidate;
                        }
                    }
                    int bestVotes = -1;
                    for (int i = 0; i < count; ++i) {
                        int votes = 0;
                        for (int j = 0; j < count; ++j) votes += candidates[j] == candidates[i];
                        if (votes > bestVotes) { bestVotes = votes; value = candidates[i]; }
                    }
                }
                if (format == QImage::Format_Grayscale16)
                    reinterpret_cast<quint16*>(out.scanLine(row))[x] = static_cast<quint16>(value);
                else
                    out.scanLine(row)[x] = static_cast<uchar>(value);
            }
        }
    }
    if (repairedRows) *repairedRows = repaired;
    return out;
}

QImage medianBlur(const QImage& input, int kernelSize, int* filteredPixels) {
    if (filteredPixels) *filteredPixels = 0;
    if (!medianFilterEnabled()) return input.convertToFormat(QImage::Format_Grayscale8);
    const QImage src = input.convertToFormat(QImage::Format_Grayscale8);
    QImage out = src;
    int k = std::clamp(kernelSize, 3, 11);
    if ((k & 1) == 0) ++k;
    const int radius = k / 2;
    if (src.width() <= 2 * radius || src.height() <= 2 * radius) return out;
    std::array<int, 121> values{};
    const int count = k * k;
    const int middle = count / 2;
    for (int y = 0; y < src.height(); ++y) {
        uchar* dst = out.scanLine(y);
        for (int x = 0; x < src.width(); ++x) {
            int n = 0;
            for (int dy = -radius; dy <= radius; ++dy)
                for (int dx = -radius; dx <= radius; ++dx)
                    values[static_cast<std::size_t>(n++)] = gray(
                        src, std::clamp(x + dx, 0, src.width() - 1),
                        std::clamp(y + dy, 0, src.height() - 1));
            auto begin = values.begin();
            auto median = begin + middle;
            std::nth_element(begin, median, begin + count);
            const int filtered = *median;
            if (dst[x] != filtered) {
                dst[x] = static_cast<uchar>(filtered);
                if (filteredPixels) ++*filteredPixels;
            }
        }
    }
    return out;
}

QImage medianBlurPreservingFormat(const QImage& input, int kernelSize, int* filteredPixels,
                                 bool preserveWhiteNoData, quint16 gray16NoData) {
    if (filteredPixels) *filteredPixels = 0;
    if (input.isNull()) return {};
    if (!medianFilterEnabled()) return input.copy();
    const int k = std::clamp(kernelSize | 1, 3, 11);
    const int radius = k / 2;
    const int width = input.width(), height = input.height();
    if (width <= 2 * radius || height <= 2 * radius) return input.copy();
    const auto format = input.format();
    const bool gray16 = format == QImage::Format_Grayscale16;
    const bool indexed = format == QImage::Format_Indexed8;
    const bool gray8 = format == QImage::Format_Grayscale8;
    if (!gray16 && !indexed && !gray8 && format != QImage::Format_RGB32 &&
        format != QImage::Format_ARGB32 && format != QImage::Format_ARGB32_Premultiplied &&
        format != QImage::Format_RGB888) {
        return medianBlurPreservingFormat(input.convertToFormat(QImage::Format_RGB32), k, filteredPixels);
    }
    QImage src = (gray16 || indexed || gray8) ? input.copy() : input.convertToFormat(QImage::Format_RGB32);
    QImage out = src.copy();
    std::array<int, 121> values{};
    std::array<int, 121> red{}, green{}, blue{};
    const int count = k * k;
    const int middle = count / 2;
    auto median = [middle](std::array<int, 121>& samples, int n) {
        auto mid = samples.begin() + n / 2;
        std::nth_element(samples.begin(), mid, samples.begin() + n);
        return *mid;
    };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int n = 0;
            for (int dy = -radius; dy <= radius; ++dy) {
                const int sy = std::clamp(y + dy, 0, height - 1);
                for (int dx = -radius; dx <= radius; ++dx) {
                    const int sx = std::clamp(x + dx, 0, width - 1);
                    if (gray16) values[static_cast<std::size_t>(n++)] =
                        reinterpret_cast<const quint16*>(src.constScanLine(sy))[sx];
                    else if (indexed || gray8) values[static_cast<std::size_t>(n++)] = src.constScanLine(sy)[sx];
                    else {
                        const QRgb pixel = reinterpret_cast<const QRgb*>(src.constScanLine(sy))[sx];
                        red[static_cast<std::size_t>(n)] = qRed(pixel);
                        green[static_cast<std::size_t>(n)] = qGreen(pixel);
                        blue[static_cast<std::size_t>(n)] = qBlue(pixel);
                        ++n;
                    }
                }
            }
            if (gray16) {
                const quint16 oldValue = reinterpret_cast<const quint16*>(src.constScanLine(y))[x];
                quint16 filteredValue = oldValue;
                if (oldValue != gray16NoData) {
                    // Exclude only the no-data value declared by this raster.
                    // Zero is a valid visible reflectance-factor value, so it
                    // must not be treated as a universal sentinel. Filter only
                    // when valid neighbours form a strict majority; otherwise
                    // preserve the centre sample.
                    int validCount = 0;
                    for (int i = 0; i < count; ++i) {
                        const int value = values[static_cast<std::size_t>(i)];
                        if (value != gray16NoData)
                            values[static_cast<std::size_t>(validCount++)] = value;
                    }
                    if (validCount > count / 2)
                        filteredValue = static_cast<quint16>(median(values, validCount));
                }
                const quint16 preservedNoData = filteredValue;
                reinterpret_cast<quint16*>(out.scanLine(y))[x] = preservedNoData;
                if (filteredPixels && oldValue != preservedNoData && oldValue != gray16NoData) ++*filteredPixels;
            } else if (indexed) {
                const uchar oldValue = src.constScanLine(y)[x];
                int best = oldValue, bestVotes = -1;
                for (int i = 0; i < count; ++i) {
                    const int candidate = values[static_cast<std::size_t>(i)];
                    if (candidate == 255) continue;
                    int votes = 0;
                    for (int j = 0; j < count; ++j) votes += values[static_cast<std::size_t>(j)] == candidate;
                    if (votes > bestVotes) { bestVotes = votes; best = candidate; }
                }
                if (oldValue != 255 || bestVotes >= 0) out.scanLine(y)[x] = static_cast<uchar>(best);
                if (filteredPixels && oldValue != out.constScanLine(y)[x]) ++*filteredPixels;
            } else if (gray8) {
                const uchar oldValue = src.constScanLine(y)[x];
                const uchar newValue = preserveWhiteNoData && oldValue == 255
                    ? oldValue : static_cast<uchar>(median(values, count));
                out.scanLine(y)[x] = newValue;
                if (filteredPixels && oldValue != newValue) ++*filteredPixels;
            } else {
                const QRgb oldValue = reinterpret_cast<const QRgb*>(src.constScanLine(y))[x];
                const QRgb newValue = qRgb(median(red, count), median(green, count), median(blue, count));
                reinterpret_cast<QRgb*>(out.scanLine(y))[x] = newValue;
                if (filteredPixels && oldValue != newValue) ++*filteredPixels;
            }
        }
    }
    return out;
}

QImage repairImpulseNoise(const QImage& input, int radius, int threshold, int* repairedPixels) {
    Q_UNUSED(threshold);
    return medianBlur(input, 2 * std::clamp(radius, 1, 5) + 1, repairedPixels);
}

QImage destripeConservative(const QImage& input, float z) {
    QImage src = input.convertToFormat(QImage::Format_Grayscale8);
    QImage out = src;
    if (src.width() < 8) return out;
    QVector<float> means(src.width(), 0.0f);
    for (int x = 0; x < src.width(); ++x) {
        double s = 0;
        for (int y = 0; y < src.height(); y += std::max(1, src.height() / 512)) s += gray(src, x, y);
        means[x] = static_cast<float>(s / std::ceil(src.height() / static_cast<double>(std::max(1, src.height() / 512))));
    }
    float mean = 0.0f;
    for (float v : means) mean += v;
    mean /= static_cast<float>(std::max<qsizetype>(1, means.size()));
    float var = 0.0f;
    for (float v : means) var += (v - mean) * (v - mean);
    var = std::sqrt(var / static_cast<float>(std::max<qsizetype>(1, means.size())));
    for (int x = 0; x < src.width(); ++x) {
        const float d = means[x] - mean;
        if (std::fabs(d) < z * std::max(var, 1.0f)) continue;
        for (int y = 0; y < src.height(); ++y) {
            const int nv = qBound(0, static_cast<int>(std::lround(gray(src, x, y) - 0.55f * d)), 255);
            out.setPixel(x, y, nv);
        }
    }
    return out;
}


QImage falseColor(const QImage& visible, const QImage& ir4, const QImage& ir1,
                  const ScanNavigation* navigation, QString* geographyReport) {
    // SatDump's "Legacy S-VISSR False Color": lut(ch2, 1-ch1, 0), lut(ch2, 1-ch1, 1), lut(ch2, 1-ch1, 2)
    // with channel 1 (visible) as the reference grid. ir4 is S-VISSR channel 2 (3.75 um) and its
    // coordinate transform onto the visible grid is 4x in X and Y. The table colour is written
    // out as it is: no sea/land placement, no limb haze, no disk mask and no colour correction.
    // (ir1 and navigation are not used any more; the signature is kept for the callers.)
    Q_UNUSED(ir1);
    Q_UNUSED(navigation);
    if (geographyReport) *geographyReport = QStringLiteral("SatDump legacy S-VISSR colour table, lut(ch2, 1-ch1)");
    if (visible.isNull() || ir4.isNull()) return {};

    const QImage vis = visible.convertToFormat(QImage::Format_Grayscale8);
    const QImage ir = ir4.convertToFormat(QImage::Format_Grayscale8);
    // Keep the native 100x256 resource dimensions. SatDump scales normalised input channels onto
    // [0, width-1] / [0, height-1] and reads integer LUT coordinates; resizing this table to
    // 256x256 changes its colour mapping.
    static const QImage lut = QImage(QCoreApplication::applicationDirPath() +
        QStringLiteral("/resources/fy2/svissr/lut.png")).convertToFormat(QImage::Format_RGB32);
    if (lut.isNull()) return {};

    auto sampleGray = [](const QImage& image, double x, double y) {
        x = std::clamp(x, 0.0, static_cast<double>(image.width() - 1));
        y = std::clamp(y, 0.0, static_cast<double>(image.height() - 1));
        const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
        const int x1 = std::min(x0 + 1, image.width() - 1);
        const int y1 = std::min(y0 + 1, image.height() - 1);
        const double fx = x - x0, fy = y - y0;
        const auto* row0 = image.constScanLine(y0);
        const auto* row1 = image.constScanLine(y1);
        const double top = row0[x0] * (1.0 - fx) + row0[x1] * fx;
        const double bot = row1[x0] * (1.0 - fx) + row1[x1] * fx;
        return top * (1.0 - fy) + bot * fy;
    };
    auto sampleLut = [](double channel2, double channel1) {
        const int x = std::clamp(static_cast<int>(channel2 / 255.0 * (lut.width() - 1)),
                                 0, lut.width() - 1);
        const int y = std::clamp(static_cast<int>(channel1 / 255.0 * (lut.height() - 1)),
                                 0, lut.height() - 1);
        return lut.pixel(x, y);
    };

    QImage out(vis.size(), QImage::Format_RGB32);
    for (int y = 0; y < vis.height(); ++y) {
        const uchar* visRow = vis.constScanLine(y);
        auto* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < vis.width(); ++x) {
            // The channel transform maps visible coordinates to IR by 4x.
            const double irValue = sampleGray(ir, x / 4.0, y / 4.0);
            const double visValue = visRow[x];
            dst[x] = sampleLut(irValue, 255.0 - visValue);
        }
    }
    return out;
}
QImage visibleReflectanceFactor(const QImage& visible) {
    if (visible.isNull()) return {};
    QImage out(visible.size(), QImage::Format_Grayscale16);
    for (int y = 0; y < visible.height(); ++y) {
        auto* dst = reinterpret_cast<quint16*>(out.scanLine(y));
        for (int x = 0; x < visible.width(); ++x) {
            // Hundredths of one reflectance percent. 65535 remains available
            // for a future explicit fill mask; legacy inputs cannot distinguish
            // missing fill from true DN=0, so retain DN=0 as calibrated zero.
            dst[x] = static_cast<quint16>(qRound(nadirStationVisibleReflectance[visibleDn(visible, x, y)] * 100.0));
        }
    }
    return out;
}

// Full-disk framing of an S-VISSR product.
//
// The decoded visible raster is stored four times taller than the infrared one
// because the assembly stacks the four visible detector lines of each scan
// position. The *horizontal* extent of both products is the full 2,291-sample
// S-VISSR line: the visible payload delivers 2,291-sample copies of that line,
// so the visible raster's 9,164-pixel width is an interpolation to four detector
// lines per scan position rather than a larger scan. This function therefore
// reports the raster width as the full-disk size for both channels and derives
// the scan-position count from the height for the visible assembly.
struct FY2DiskFraming {
    int fullDiskSize = 2291;
    int scanPositions = 2291;
};

FY2DiskFraming fy2DiskFraming(const QImage& image, int sourceFirstLine) {
    FY2DiskFraming framing;
    const int w = image.width();
    const int h = image.height();
    if (w <= 0) return framing;
    framing.fullDiskSize = w;
    const int completeVisLines = std::max(1, qMax(1, sourceFirstLine) - 1) * 4;
    const bool visibleAssembly = h >= 2 * w && h >= completeVisLines;
    framing.scanPositions = visibleAssembly ? h / 4 : w;
    return framing;
}

namespace {

constexpr double kPi = 3.14159265358979323846;

// Solar declination (degrees) for a day of year, Cooper's approximation. Good to
// about 0.5 degrees, which is far below the pixel size of the regional grid.
double solarDeclinationDeg(int dayOfYear) {
    return 23.45 * std::sin(2.0 * kPi * (284.0 + dayOfYear) / 365.0);
}

// Greenwich mean sidereal time in degrees.
double greenwichSiderealDeg(const QDateTime& utc) {
    const QDateTime time = utc.toUTC();
    const double hours = time.time().hour() + time.time().minute() / 60.0 +
                         time.time().second() / 3600.0 + time.time().msec() / 3600000.0;
    const double jd = time.date().toJulianDay() + 0.5; // 0h UT of that date
    const double t = (jd - 2451545.0) / 36525.0;
    double gmst = 280.46061837 + 360.98564736629 * (jd - 2451545.0) +
                  0.000387933 * t * t - t * t * t / 38710000.0;
    gmst = std::fmod(gmst + 15.0 * hours, 360.0);
    if (gmst < 0.0) gmst += 360.0;
    return gmst;
}

double solarZenithCosine(double latitudeRad, double longitudeRad, const QDateTime& utc) {
    const QDateTime time = utc.toUTC();
    const double hour = time.time().hour() + time.time().minute() / 60.0 +
        time.time().second() / 3600.0 + time.time().msec() / 3600000.0;
    const double gamma = 2.0 * kPi / 365.0 *
        (time.date().dayOfYear() - 1 + (hour - 12.0) / 24.0);
    const double equationMinutes = 229.18 * (0.000075 + 0.001868 * std::cos(gamma) -
        0.032077 * std::sin(gamma) - 0.014615 * std::cos(2.0 * gamma) -
        0.040849 * std::sin(2.0 * gamma));
    const double declination = 0.006918 - 0.399912 * std::cos(gamma) +
        0.070257 * std::sin(gamma) - 0.006758 * std::cos(2.0 * gamma) +
        0.000907 * std::sin(2.0 * gamma) - 0.002697 * std::cos(3.0 * gamma) +
        0.001480 * std::sin(3.0 * gamma);
    double solarMinutes = std::fmod(hour * 60.0 + equationMinutes +
                                    4.0 * longitudeRad * 180.0 / kPi, 1440.0);
    if (solarMinutes < 0.0) solarMinutes += 1440.0;
    const double hourAngle = (solarMinutes / 4.0 - 180.0) * kPi / 180.0;
    return std::sin(latitudeRad) * std::sin(declination) +
           std::cos(latitudeRad) * std::cos(declination) * std::cos(hourAngle);
}

} // namespace

QImage visibleReflectanceNormalised(const QImage& visible, double subSatelliteLongitudeDeg,
                                    const QDateTime& observationUtc) {
    if (visible.isNull() || !observationUtc.isValid()) return {};
    // The satellite longitude defines the sub-satellite point; the scan geometry
    // below is expressed in the satellite's own Earth-fixed frame, so this value
    // is recorded for the sidecar rather than needed by the intersection.
    Q_UNUSED(subSatelliteLongitudeDeg);
    const double declination = solarDeclinationDeg(observationUtc.date().dayOfYear()) * kPi / 180.0;
    const double gmstDeg = greenwichSiderealDeg(observationUtc);
    constexpr double aKm = 6378.137;
    constexpr double bKm = 6356.752314;
    constexpr double satRadiusKm = 42164.0;
    const double eccentricity2 = 1.0 - (bKm * bKm) / (aKm * aKm);

    const QImage gray = visible.convertToFormat(QImage::Format_Grayscale8);
    const FY2DiskFraming framing = fy2DiskFraming(gray, 1);
    QImage out(gray.size(), QImage::Format_Grayscale16);
    out.fill(65535);

    const double satX = satRadiusKm * std::cos(0.0), satY = 0.0, satZ = 0.0;

    const double halfX = std::asin(aKm / satRadiusKm);
    const double halfY = std::asin(bKm / satRadiusKm);

    for (int row = 0; row < framing.scanPositions; ++row) {
        // Scan position -> view angle -> ground longitude/latitude of the nadir
        // track centre of that scan line. The visible assembly stores four
        // detector lines per scan position.
        const double scanY = (1.0 - 2.0 * row / std::max(1, framing.scanPositions - 1)) * halfY;
        for (int col = 0; col < framing.fullDiskSize; ++col) {
            const double scanX = (2.0 * col / std::max(1, framing.fullDiskSize - 1) - 1.0) * halfX;
            // Forward intersection of the scan ray with the WGS-84 ellipsoid.
            const double cosY = std::cos(scanY), sinY = std::sin(scanY);
            const double cosX = std::cos(scanX), sinX = std::sin(scanX);
            const double dx = sinX * cosY, dy = -cosX * cosY, dz = sinY;
            const double bx = -satX, by = -satY, bz = -satZ;
            const double A = dx * dx / (aKm * aKm) + dy * dy / (aKm * aKm) + dz * dz / (bKm * bKm);
            const double B = 2.0 * (bx * dx / (aKm * aKm) + by * dy / (aKm * aKm) + bz * dz / (bKm * bKm));
            const double C = bx * bx / (aKm * aKm) + by * by / (aKm * aKm) + bz * bz / (bKm * bKm) - 1.0;
            const double discriminant = B * B - 4.0 * A * C;
            if (discriminant <= 0.0 || A <= 0.0) continue;
            const double t = (-B - std::sqrt(discriminant)) / (2.0 * A);
            if (!(t > 0.0)) continue;
            const double px = satX + t * dx;
            const double py = satY + t * dy;
            const double pz = satZ + t * dz;
            const double lon = std::atan2(py, px);
            const double lat = std::atan2(pz, std::sqrt(px * px + py * py) * (1.0 - eccentricity2));
            // Local solar hour angle and zenith angle.
            const double hourAngle = gmstDeg * kPi / 180.0 + lon - kPi;
            const double cosZenith = std::sin(lat) * std::sin(declination) +
                std::cos(lat) * std::cos(declination) * std::cos(hourAngle);
            if (cosZenith <= std::cos(70.0 * kPi / 180.0)) continue; // night or grazing sun
            const int sx = col;
            for (int sensor = 0; sensor < 4; ++sensor) {
                const int sy = row * 4 + sensor;
                if (sy >= gray.height() || sx >= gray.width()) continue;
                const int dn = gray.constScanLine(sy)[sx];
                const double reflectance = nadirStationVisibleReflectance[static_cast<std::size_t>(dn)] / cosZenith;
                auto* dst = reinterpret_cast<quint16*>(out.scanLine(sy));
                dst[sx] = static_cast<quint16>(qBound(0, qRound(reflectance * 100.0), 65534));
            }
        }
    }
    return out;
}

QImage visibleReflectanceEnhancement(const QImage& visible) {    if (visible.isNull()) return {};
    QImage out(visible.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < visible.height(); ++y) {
        uchar* dst = out.scanLine(y);
        for (int x = 0; x < visible.width(); ++x) {
            const double pct = nadirStationVisibleReflectance[visibleDn(visible, x, y)];
            // Monotonic display mapping; calibrated raster remains the source
            // of the measurement and values above 100% are visibly clipped.
            dst[x] = static_cast<uchar>(qBound(0, qRound(std::sqrt(std::clamp(pct / 100.0, 0.0, 1.0)) * 255.0), 255));
        }
    }
    return out;
}

QImage visibleReflectanceColor(const QImage& visible) {
    if (visible.isNull()) return {};
    QImage out(visible.size(), QImage::Format_Indexed8);
    QVector<QRgb> palette;
    palette.reserve(64);
    for (const double reflectance : nadirStationVisibleReflectance) palette.push_back(reflectancePalette(reflectance));
    out.setColorTable(palette);
    for (int y = 0; y < visible.height(); ++y) {
        auto* dst = out.scanLine(y);
        for (int x = 0; x < visible.width(); ++x) dst[x] = static_cast<uchar>(visibleDn(visible, x, y));
    }
    return out;
}

namespace {

// Half-angle of the full disk as seen from geostationary altitude, and the
// angular step of the nominal 2,291-sample S-VISSR lattice. Falling back to
// these values reproduces the pre-calibration projection exactly.
constexpr double kGeostationarySemiMajorKm = 6378.137;
constexpr double kGeostationaryRadiusKm = 42164.0;
constexpr double kNominalScanStepRad = 1.4e-4;
// Width of the infrared lattice the scan residuals are expressed in. The visible raster is four times
// wider (9164), so a residual measured on the infrared grid is four times larger there.
constexpr double kResidualLatticeSize = 2291.0;

double nominalHalfAngle(double semiMajorKm) {
    return std::asin(std::clamp(semiMajorKm / kGeostationaryRadiusKm, 0.0, 1.0));
}

} // namespace

ScanGeometry scanGeometryFor(const QString& noradId) {
    ScanGeometry geometry;
    const nadirstation::orbit::ScanCalibration calibration = nadirstation::orbit::scanCalibrationFor(noradId);
    if (!calibration.valid) return geometry;
    if (calibration.angularStepScale > 0.0) geometry.stepScale = calibration.angularStepScale;
    if (calibration.residualValidated || calibration.residualAppliedPendingValidation) {
        geometry.residualRowPx = calibration.residualRowPx;
        geometry.residualColPx = calibration.residualColPx;
    }
    return geometry;
}

// ---------------------------------------------------------------------------------------------
// Per-observation spin-scan navigation
// ---------------------------------------------------------------------------------------------
namespace {

constexpr double kNavLattice = 2291.0;            // infrared full-disk lattice
constexpr double kNavOrigin = (kNavLattice - 1.0) * 0.5;
constexpr double kNavA = 6378.137;
constexpr double kNavB = 6356.752314;
constexpr double kNavE2 = 1.0 - (kNavB * kNavB) / (kNavA * kNavA);

// Sub-pixel position where `values` first rises above `threshold`, walking from index 0 with the
// given stride. -1 when it never does or when the disk already touches the raster edge.
double firstCrossing(const quint16* values, int count, std::ptrdiff_t stride, double threshold) {
    for (int i = 0; i < count; ++i) {
        const double v = values[i * stride];
        if (v > threshold) {
            if (i == 0) return -1.0;
            // A crossing preceded by no-data (0 = line or pixel not received) is a cut in the
            // reception, not the limb: space reads ~121 K, never 0. Partial receptions produce
            // whole rows of such cuts, which otherwise pull the disk fit by hundreds of pixels.
            for (int k = 1; k <= 3 && i - k >= 0; ++k)
                if (values[(i - k) * stride] == 0) return -1.0;
            const double previous = values[(i - 1) * stride];
            const double denominator = v - previous;
            return denominator > 0.0 ? (i - 1) + (threshold - previous) / denominator : i;
        }
    }
    return -1.0;
}

// Solves the 5x5 normal equations of the conic A x^2 + B xy + C y^2 + D x + E y = 1.
bool solveConic(const std::vector<double>& xs, const std::vector<double>& ys,
                const std::vector<char>& keep, std::array<double, 5>& out) {
    double m[5][6] = {};
    for (std::size_t i = 0; i < xs.size(); ++i) {
        if (!keep[i]) continue;
        const double x = xs[i], y = ys[i];
        const double row[5] = {x * x, x * y, y * y, x, y};
        for (int a = 0; a < 5; ++a) {
            for (int b = 0; b < 5; ++b) m[a][b] += row[a] * row[b];
            m[a][5] += row[a];
        }
    }
    for (int c = 0; c < 5; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 5; ++r) if (std::fabs(m[r][c]) > std::fabs(m[pivot][c])) pivot = r;
        if (std::fabs(m[pivot][c]) < 1e-300) return false;
        if (pivot != c) for (int k = 0; k < 6; ++k) std::swap(m[c][k], m[pivot][k]);
        for (int r = 0; r < 5; ++r) {
            if (r == c) continue;
            const double f = m[r][c] / m[c][c];
            for (int k = c; k < 6; ++k) m[r][k] -= f * m[c][k];
        }
    }
    for (int c = 0; c < 5; ++c) out[static_cast<std::size_t>(c)] = m[c][5] / m[c][c];
    return true;
}

// Centre, semi-axes and shear of the conic (coordinates are whatever the fit used).
bool conicGeometry(const std::array<double, 5>& q, double& cx, double& cy, double& rx, double& ry,
                   double& shear) {
    const double A = q[0], B = q[1], C = q[2], D = q[3], E = q[4];
    const double det = 4.0 * A * C - B * B;
    if (!(det > 0.0) || !(A > 0.0)) return false;
    cx = (-2.0 * C * D + B * E) / det;
    cy = (-2.0 * A * E + B * D) / det;
    const double k = A * cx * cx + B * cx * cy + C * cy * cy + D * cx + E * cy;
    const double g = 1.0 - k;
    shear = -B / (2.0 * A);
    const double cPrime = C - B * B / (4.0 * A);
    if (!(g > 0.0) || !(cPrime > 0.0)) return false;
    rx = std::sqrt(g / A);
    ry = std::sqrt(g / cPrime);
    return std::isfinite(rx) && std::isfinite(ry);
}

struct NavFrame {
    double px, py, pz, distance;
    double ux, uy, uz;   // up (satellite direction)
    double nx, ny, nz;   // scan north (spin axis projected normal to up)
    double ex, ey, ez;   // scan east = north x up
};

NavFrame navFrame(const nadirstation::orbit::SatellitePosition& s) {
    NavFrame f{};
    f.px = s.xKm; f.py = s.yKm; f.pz = s.zKm;
    f.distance = std::sqrt(f.px * f.px + f.py * f.py + f.pz * f.pz);
    f.ux = f.px / f.distance; f.uy = f.py / f.distance; f.uz = f.pz / f.distance;
    double hx = s.orbitNormalX, hy = s.orbitNormalY, hz = s.orbitNormalZ;
    if (!s.hasOrbitNormal) { hx = 0.0; hy = 0.0; hz = 1.0; }
    const double dot = hx * f.ux + hy * f.uy + hz * f.uz;
    f.nx = hx - dot * f.ux; f.ny = hy - dot * f.uy; f.nz = hz - dot * f.uz;
    const double nn = std::sqrt(f.nx * f.nx + f.ny * f.ny + f.nz * f.nz);
    f.nx /= nn; f.ny /= nn; f.nz /= nn;
    f.ex = f.ny * f.uz - f.nz * f.uy;
    f.ey = f.nz * f.ux - f.nx * f.uz;
    f.ez = f.nx * f.uy - f.ny * f.ux;
    return f;
}

double residualAt(const std::array<double, 3>& c, double row, double col) {
    return c[0] + c[1] * (col - kNavOrigin) + c[2] * (row - kNavOrigin);
}

QString findObservationManifest(const QDir& directory) {
    for (const char* name : {"FY2_Observation.json", "FY2G_Observation.json", "FY2H_Observation.json",
                             "MGAS_Observation.json"}) {
        const QString path = directory.filePath(QString::fromLatin1(name));
        if (QFile::exists(path)) return path;
    }
    return {};
}

} // namespace

DiskGeometry measureDiskGeometry(const QImage& ir1Tbb01K, double thresholdKelvin) {
    DiskGeometry disk;
    disk.thresholdKelvin = thresholdKelvin;
    if (ir1Tbb01K.isNull()) return disk;
    const QImage tbb = ir1Tbb01K.format() == QImage::Format_Grayscale16
        ? ir1Tbb01K : ir1Tbb01K.convertToFormat(QImage::Format_Grayscale16);
    const int w = tbb.width(), h = tbb.height();
    if (w < 512 || h < 512) return disk;
    const double threshold = thresholdKelvin * 100.0;
    const auto* base = reinterpret_cast<const quint16*>(tbb.constBits());
    const std::ptrdiff_t stride = tbb.bytesPerLine() / static_cast<std::ptrdiff_t>(sizeof(quint16));
    // Limb points in coordinates normalised to [-1, 1] for a well-conditioned fit.
    const double ox = (w - 1) * 0.5, oy = (h - 1) * 0.5, scale = std::max(ox, oy);
    std::vector<double> xs, ys;
    xs.reserve(8192); ys.reserve(8192);
    auto enoughDisk = [&](const quint16* p, int count, std::ptrdiff_t step) {
        int above = 0;
        for (int i = 0; i < count && above < 20; ++i) if (p[i * step] > threshold) ++above;
        return above >= 20;
    };
    std::vector<quint16> reversed(static_cast<std::size_t>(std::max(w, h)));
    for (int y = 0; y < h; y += 2) {
        const quint16* row = base + y * stride;
        if (!enoughDisk(row, w, 1)) continue;
        const double left = firstCrossing(row, w, 1, threshold);
        for (int x = 0; x < w; ++x) reversed[static_cast<std::size_t>(x)] = row[w - 1 - x];
        const double right = firstCrossing(reversed.data(), w, 1, threshold);
        if (left >= 0.0) { xs.push_back((left - ox) / scale); ys.push_back((y - oy) / scale); }
        if (right >= 0.0) { xs.push_back((w - 1 - right - ox) / scale); ys.push_back((y - oy) / scale); }
    }
    for (int x = 0; x < w; x += 2) {
        const quint16* column = base + x;
        if (!enoughDisk(column, h, stride)) continue;
        const double top = firstCrossing(column, h, stride, threshold);
        for (int y = 0; y < h; ++y) reversed[static_cast<std::size_t>(y)] = column[(h - 1 - y) * stride];
        const double bottom = firstCrossing(reversed.data(), h, 1, threshold);
        if (top >= 0.0) { xs.push_back((x - ox) / scale); ys.push_back((top - oy) / scale); }
        if (bottom >= 0.0) { xs.push_back((x - ox) / scale); ys.push_back((h - 1 - bottom - oy) / scale); }
    }
    if (xs.size() < 1000) return disk;
    std::vector<char> keep(xs.size(), 1);
    std::array<double, 5> q{};
    double cx = 0, cy = 0, rx = 0, ry = 0, shear = 0, rms = 0;
    int accepted = 0;
    std::vector<double> residual(xs.size()), absolute;
    for (int iteration = 0; iteration < 8; ++iteration) {
        if (!solveConic(xs, ys, keep, q) || !conicGeometry(q, cx, cy, rx, ry, shear)) return disk;
        const double meanRadius = 0.5 * (rx + ry) * scale;
        absolute.clear();
        for (std::size_t i = 0; i < xs.size(); ++i) {
            const double u = (xs[i] - cx - shear * (ys[i] - cy)) / rx;
            const double v = (ys[i] - cy) / ry;
            residual[i] = (std::sqrt(u * u + v * v) - 1.0) * meanRadius;
            if (keep[i]) absolute.push_back(std::fabs(residual[i]));
        }
        std::nth_element(absolute.begin(), absolute.begin() + static_cast<std::ptrdiff_t>(absolute.size() / 2),
                         absolute.end());
        const double sigma = 1.4826 * absolute[absolute.size() / 2];
        const double limit = std::max(3.0 * sigma, 0.75);
        accepted = 0;
        double sum2 = 0.0;
        for (std::size_t i = 0; i < xs.size(); ++i) {
            keep[i] = std::fabs(residual[i]) < limit ? 1 : 0;
            if (keep[i]) { ++accepted; sum2 += residual[i] * residual[i]; }
        }
        rms = accepted > 0 ? std::sqrt(sum2 / accepted) : 0.0;
        if (accepted < 1000) return disk;
    }
    disk.cx = cx * scale + ox;
    disk.cy = cy * scale + oy;
    disk.rx = rx * scale;
    disk.ry = ry * scale;
    disk.shear = shear;            // dimensionless (px per px), unchanged by the uniform scaling
    disk.rmsPx = rms;
    disk.points = accepted;
    // Plausibility for an S-VISSR infrared full disk: radius ~1075 px, sub-pixel limb fit.
    disk.valid = rms < 2.0 && std::fabs(shear) < 0.05 &&
                 disk.rx > 0.40 * w && disk.rx < 0.50 * w && disk.ry > 0.40 * w && disk.ry < 0.50 * w;
    return disk;
}

nadirstation::orbit::SatellitePosition navigationSatelliteAtLine(const ScanNavigation& navigation, double line) {
    if (navigation.knots.isEmpty()) return {};
    if (navigation.knots.size() == 1) return navigation.knots.first();
    const double t = std::clamp((line - navigation.knotFirstLine) / navigation.knotLineStep, 0.0,
                                static_cast<double>(navigation.knots.size() - 1));
    const int i = std::min(static_cast<int>(t), static_cast<int>(navigation.knots.size()) - 2);
    const double f = t - i;
    const auto& a = navigation.knots[i];
    const auto& b = navigation.knots[i + 1];
    nadirstation::orbit::SatellitePosition s = a;
    s.xKm = a.xKm + f * (b.xKm - a.xKm);
    s.yKm = a.yKm + f * (b.yKm - a.yKm);
    s.zKm = a.zKm + f * (b.zKm - a.zKm);
    s.distanceKm = std::sqrt(s.xKm * s.xKm + s.yKm * s.yKm + s.zKm * s.zKm);
    s.orbitNormalX = a.orbitNormalX + f * (b.orbitNormalX - a.orbitNormalX);
    s.orbitNormalY = a.orbitNormalY + f * (b.orbitNormalY - a.orbitNormalY);
    s.orbitNormalZ = a.orbitNormalZ + f * (b.orbitNormalZ - a.orbitNormalZ);
    s.subpointLongitudeDeg = std::atan2(s.yKm, s.xKm) * 180.0 / kPi;
    s.subpointLatitudeDeg = std::atan2(s.zKm, std::hypot(s.xKm, s.yKm)) * 180.0 / kPi;
    return s;
}

bool navigationLatLonToPixel(const ScanNavigation& navigation, double latitudeDeg,
                             double longitudeDeg, double& row, double& col) {
    if (!navigation.valid) return false;
    const double lat = latitudeDeg * kPi / 180.0, lon = longitudeDeg * kPi / 180.0;
    const double sinLat = std::sin(lat), cosLat = std::cos(lat);
    const double sinLon = std::sin(lon), cosLon = std::cos(lon);
    const double n = kNavA / std::sqrt(1.0 - kNavE2 * sinLat * sinLat);
    const double gx = n * cosLat * cosLon, gy = n * cosLat * sinLon, gz = n * (1.0 - kNavE2) * sinLat;
    const DiskGeometry& d = navigation.disk;
    double line = 0.5 * (navigation.scanLineMin + navigation.scanLineMax);
    for (int iteration = 0; iteration < 3; ++iteration) {
        const NavFrame f = navFrame(navigationSatelliteAtLine(navigation, line));
        const double tx = gx - f.px, ty = gy - f.py, tz = gz - f.pz;
        // The point must face the satellite (ellipsoid normal towards it).
        if ((-tx) * cosLat * cosLon + (-ty) * cosLat * sinLon + (-tz) * sinLat <= 0.0) return false;
        const double forward = -(tx * f.ux + ty * f.uy + tz * f.uz);
        if (forward <= 0.0) return false;
        const double e = tx * f.ex + ty * f.ey + tz * f.ez;
        const double nn = tx * f.nx + ty * f.ny + tz * f.nz;
        const double scanX = std::atan2(e, forward);
        const double scanY = std::atan2(nn, std::hypot(forward, e));
        const double stepX = std::asin(std::clamp(kNavA / f.distance, 0.0, 1.0)) / d.rx;
        const double stepY = std::asin(std::clamp(kNavB / f.distance, 0.0, 1.0)) / d.ry;
        const double rowModel = d.cy - scanY / stepY;
        const double colModel = d.cx + scanX / stepX + d.shear * (rowModel - d.cy);
        row = rowModel + residualAt(navigation.residualRow, rowModel, colModel);
        col = colModel + residualAt(navigation.residualCol, rowModel, colModel);
        line = navigation.scanLineMin + row;   // line of the full-disk row, 1-based
    }
    return std::isfinite(row) && std::isfinite(col);
}

bool navigationPixelToLatLon(const ScanNavigation& navigation, double row, double col,
                             double& latitudeDeg, double& longitudeDeg) {
    if (!navigation.valid) return false;
    const DiskGeometry& d = navigation.disk;
    const double rowModel = row - residualAt(navigation.residualRow, row, col);
    const double colModel = col - residualAt(navigation.residualCol, row, col);
    const NavFrame f = navFrame(navigationSatelliteAtLine(navigation, navigation.scanLineMin + row));
    const double stepX = std::asin(std::clamp(kNavA / f.distance, 0.0, 1.0)) / d.rx;
    const double stepY = std::asin(std::clamp(kNavB / f.distance, 0.0, 1.0)) / d.ry;
    const double ax = (colModel - d.cx - d.shear * (rowModel - d.cy)) * stepX;
    const double ay = (d.cy - rowModel) * stepY;
    const double ca = std::cos(ax), sa = std::sin(ax), cb = std::cos(ay), sb = std::sin(ay);
    const double dx = f.ex * sa * cb + f.nx * sb - f.ux * ca * cb;
    const double dy = f.ey * sa * cb + f.ny * sb - f.uy * ca * cb;
    const double dz = f.ez * sa * cb + f.nz * sb - f.uz * ca * cb;
    const double ia = 1.0 / (kNavA * kNavA), ib = 1.0 / (kNavB * kNavB);
    const double qa = (dx * dx + dy * dy) * ia + dz * dz * ib;
    const double qb = 2.0 * ((f.px * dx + f.py * dy) * ia + f.pz * dz * ib);
    const double qc = (f.px * f.px + f.py * f.py) * ia + f.pz * f.pz * ib - 1.0;
    const double disc = qb * qb - 4.0 * qa * qc;
    if (disc <= 0.0) return false;
    const double t = (-qb - std::sqrt(disc)) / (2.0 * qa);
    if (!(t > 0.0)) return false;
    const double x = f.px + t * dx, y = f.py + t * dy, z = f.pz + t * dz;
    longitudeDeg = std::atan2(y, x) * 180.0 / kPi;
    latitudeDeg = std::atan2(z, std::hypot(x, y) * (1.0 - kNavE2)) * 180.0 / kPi;
    return true;
}

QDateTime navigationLineTime(const ScanNavigation& navigation, double line) {
    return navigation.scanEndUtc.addMSecs(
        -qRound64((navigation.scanLineMax - line) * navigation.linePeriodS * 1000.0));
}

SstRetrieval seaSurfaceTemperatureFullDisk(const QImage& ir1Tbb01K, const QImage& ir2Tbb01K,
                                           const QImage& visible, const ScanNavigation& navigation,
                                           const SstCoefficients& k) {
    SstRetrieval out;
    if (!navigation.valid || ir1Tbb01K.isNull() || ir2Tbb01K.isNull() ||
        ir1Tbb01K.size() != ir2Tbb01K.size()) return out;
    const QImage a = ir1Tbb01K.convertToFormat(QImage::Format_Grayscale16);
    const QImage b = ir2Tbb01K.convertToFormat(QImage::Format_Grayscale16);
    const int w = a.width(), h = a.height();
    const std::size_t n = static_cast<std::size_t>(w) * h;
    std::vector<float> t11(n), t12(n);
    std::vector<char> ok(n);
    for (int y = 0; y < h; ++y) {
        const auto* ra = reinterpret_cast<const quint16*>(a.constScanLine(y));
        const auto* rb = reinterpret_cast<const quint16*>(b.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            t11[i] = ra[x] / 100.0f; t12[i] = rb[x] / 100.0f;
            ok[i] = ra[x] >= 15000 && ra[x] <= 35000 && rb[x] >= 15000 && rb[x] <= 35000;
        }
    }
    // Box mean / standard deviation with integral images (invalid samples excluded).
    auto boxStats = [&](const std::vector<float>& v, int radius, std::vector<float>& mean, std::vector<float>* sd) {
        std::vector<double> s((w + 1) * static_cast<std::size_t>(h + 1)), s2(s.size()), c(s.size());
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x, j = static_cast<std::size_t>(y + 1) * (w + 1) + x + 1;
            const double val = ok[i] ? v[i] : 0.0, cnt = ok[i] ? 1.0 : 0.0;
            s[j] = val + s[j - 1] + s[j - (w + 1)] - s[j - (w + 1) - 1];
            s2[j] = val * val + s2[j - 1] + s2[j - (w + 1)] - s2[j - (w + 1) - 1];
            c[j] = cnt + c[j - 1] + c[j - (w + 1)] - c[j - (w + 1) - 1];
        }
        mean.assign(n, 0.0f); if (sd) sd->assign(n, 1e9f);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const int x0 = std::max(0, x - radius), x1 = std::min(w - 1, x + radius);
            const int y0 = std::max(0, y - radius), y1 = std::min(h - 1, y + radius);
            auto box = [&](const std::vector<double>& t) {
                return t[static_cast<std::size_t>(y1 + 1) * (w + 1) + x1 + 1] - t[static_cast<std::size_t>(y0) * (w + 1) + x1 + 1] -
                       t[static_cast<std::size_t>(y1 + 1) * (w + 1) + x0] + t[static_cast<std::size_t>(y0) * (w + 1) + x0];
            };
            const double cnt = box(c);
            const int full = (x1 - x0 + 1) * (y1 - y0 + 1);
            if (cnt < full) continue;                       // require a complete, valid window
            const double m = box(s) / cnt;
            mean[static_cast<std::size_t>(y) * w + x] = static_cast<float>(m);
            if (sd) (*sd)[static_cast<std::size_t>(y) * w + x] =
                static_cast<float>(std::sqrt(std::max(0.0, box(s2) / cnt - m * m)));
        }
    };
    std::vector<float> m11, m12, sd11, dummy;
    boxStats(t11, 1, m11, nullptr);
    boxStats(t12, 1, m12, nullptr);
    boxStats(t11, 2, dummy, &sd11);
    // 11x11 running maximum of T11 (separable).
    std::vector<float> rowMax(n), max11(n);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        float m = 0.0f;
        for (int dx = -5; dx <= 5; ++dx) { const int xx = x + dx; if (xx >= 0 && xx < w) m = std::max(m, t11[static_cast<std::size_t>(y) * w + xx]); }
        rowMax[static_cast<std::size_t>(y) * w + x] = m;
    }
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        float m = 0.0f;
        for (int dy = -5; dy <= 5; ++dy) { const int yy = y + dy; if (yy >= 0 && yy < h) m = std::max(m, rowMax[static_cast<std::size_t>(yy) * w + x]); }
        max11[static_cast<std::size_t>(y) * w + x] = m;
    }
    // Visible reflectance (%) averaged to the infrared grid, and its 3x3 standard deviation.
    std::vector<float> vis, visSd;
    bool haveVis = false;
    if (!visible.isNull() && visible.width() % w == 0 && visible.height() % h == 0) {
        const int sx = visible.width() / w, sy = visible.height() / h;
        const QImage v = visible.format() == QImage::Format_Grayscale16 ? visible
                                                                         : visible.convertToFormat(QImage::Format_Grayscale8);
        std::vector<float> visRaw(n);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            double sum = 0.0;
            for (int j = 0; j < sy; ++j) for (int i = 0; i < sx; ++i) sum += nadirStationVisibleReflectance[static_cast<std::size_t>(visibleDn(v, x * sx + i, y * sy + j))];
            visRaw[static_cast<std::size_t>(y) * w + x] = static_cast<float>(sum / (sx * sy));
        }
        const std::vector<char> okSaved = ok;
        std::fill(ok.begin(), ok.end(), 1);
        boxStats(visRaw, 1, vis, &visSd);
        ok = okSaved;
        haveVis = true;
    }
    out.sst01K = QImage(w, h, QImage::Format_Grayscale16);
    out.sst01K.fill(65535);
    out.flags = QImage(w, h, QImage::Format_Grayscale8);
    out.flags.fill(0);
    constexpr double ea = 6378.137, eb = 6356.752314, e2 = 1.0 - (eb * eb) / (ea * ea);
    for (int y = 0; y < h; ++y) {
        auto* sstRow = reinterpret_cast<quint16*>(out.sst01K.scanLine(y));
        auto* flagRow = out.flags.scanLine(y);
        const double line = navigation.scanLineMin + y;
        const nadirstation::orbit::SatellitePosition sat = navigationSatelliteAtLine(navigation, line);
        const QDateTime when = navigationLineTime(navigation, line);
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            if (!ok[i] || m11[i] <= 0.0f || m12[i] <= 0.0f) continue;
            const double T11 = m11[i], T12 = m12[i], D = T11 - T12;
            if (T11 < 271.15 || T11 < max11[i] - 3.0 || D < -0.5 || D > 5.0) continue;
            if (sd11[i] > 0.6f) continue;
            double latDeg = 0.0, lonDeg = 0.0;
            if (!navigationPixelToLatLon(navigation, y, x, latDeg, lonDeg)) continue;
            const double la = latDeg * kPi / 180.0, lo = lonDeg * kPi / 180.0;
            const double sl = std::sin(la), cl = std::cos(la);
            const double nn = ea / std::sqrt(1.0 - e2 * sl * sl);
            const double gx = nn * cl * std::cos(lo), gy = nn * cl * std::sin(lo), gz = nn * (1.0 - e2) * sl;
            const double vx = sat.xKm - gx, vy = sat.yKm - gy, vz = sat.zKm - gz;
            const double cosSat = (vx * cl * std::cos(lo) + vy * cl * std::sin(lo) + vz * sl) /
                                  std::sqrt(vx * vx + vy * vy + vz * vz);
            if (cosSat < std::cos(60.0 * kPi / 180.0)) continue;
            const double cosSun = solarZenithCosine(la, lo, when);
            const bool day = cosSun > std::cos(80.0 * kPi / 180.0);
            bool dayChecked = false;
            if (day && haveVis) {
                const double rn = vis[i] / std::max(cosSun, 0.17);
                if (rn >= 8.0 || visSd[i] >= 1.0f) continue;
                dayChecked = true;
            }
            if (!dayChecked && sd11[i] > 0.4f) continue;     // stricter uniformity without the visible test
            const double secMinus1 = 1.0 / cosSat - 1.0;
            const double sst = k.a0 + k.a1 * T11 + k.a2 * D + k.a3 * D * secMinus1;
            if (sst < 265.0 || sst > 310.0) continue;
            sstRow[x] = static_cast<quint16>(qRound(sst * 100.0));
            flagRow[x] = dayChecked ? 1 : 2;
            if (dayChecked) ++out.dayPixels; else ++out.nightCandidates;
        }
    }
    return out;
}

namespace {
#include "fy2g_cloudtree_day.inc"

// Mean (and optionally standard deviation) over a (2r+1)^2 window of complete valid samples.
void windowStats(const std::vector<float>& v, const std::vector<char>& ok, int w, int h, int radius,
                 std::vector<float>& mean, std::vector<float>* sd) {
    const std::size_t n = static_cast<std::size_t>(w) * h;
    std::vector<double> s((w + 1) * static_cast<std::size_t>(h + 1)), s2(s.size()), c(s.size());
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const std::size_t i = static_cast<std::size_t>(y) * w + x, j = static_cast<std::size_t>(y + 1) * (w + 1) + x + 1;
        const double val = ok[i] ? v[i] : 0.0, cnt = ok[i] ? 1.0 : 0.0;
        s[j] = val + s[j - 1] + s[j - (w + 1)] - s[j - (w + 1) - 1];
        s2[j] = val * val + s2[j - 1] + s2[j - (w + 1)] - s2[j - (w + 1) - 1];
        c[j] = cnt + c[j - 1] + c[j - (w + 1)] - c[j - (w + 1) - 1];
    }
    mean.assign(n, std::numeric_limits<float>::quiet_NaN());
    if (sd) sd->assign(n, std::numeric_limits<float>::quiet_NaN());
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const int x0 = std::max(0, x - radius), x1 = std::min(w - 1, x + radius);
        const int y0 = std::max(0, y - radius), y1 = std::min(h - 1, y + radius);
        auto box = [&](const std::vector<double>& t) {
            return t[static_cast<std::size_t>(y1 + 1) * (w + 1) + x1 + 1] - t[static_cast<std::size_t>(y0) * (w + 1) + x1 + 1] -
                   t[static_cast<std::size_t>(y1 + 1) * (w + 1) + x0] + t[static_cast<std::size_t>(y0) * (w + 1) + x0];
        };
        const double cnt = box(c);
        if (cnt < (x1 - x0 + 1) * (y1 - y0 + 1)) continue;
        const double m = box(s) / cnt;
        mean[static_cast<std::size_t>(y) * w + x] = static_cast<float>(m);
        if (sd) (*sd)[static_cast<std::size_t>(y) * w + x] = static_cast<float>(std::sqrt(std::max(0.0, box(s2) / cnt - m * m)));
    }
}

void windowMax(const std::vector<float>& v, int w, int h, int radius, std::vector<float>& out) {
    std::vector<float> tmp(v.size());
    out.assign(v.size(), 0.0f);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        float m = 0.0f;
        for (int d = -radius; d <= radius; ++d) { const int xx = x + d; if (xx >= 0 && xx < w) m = std::max(m, v[static_cast<std::size_t>(y) * w + xx]); }
        tmp[static_cast<std::size_t>(y) * w + x] = m;
    }
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        float m = 0.0f;
        for (int d = -radius; d <= radius; ++d) { const int yy = y + d; if (yy >= 0 && yy < h) m = std::max(m, tmp[static_cast<std::size_t>(yy) * w + x]); }
        out[static_cast<std::size_t>(y) * w + x] = m;
    }
}

std::vector<float> tbbVector(const QImage& image, std::vector<char>& ok, bool combine) {
    const QImage g = image.convertToFormat(QImage::Format_Grayscale16);
    std::vector<float> v(static_cast<std::size_t>(g.width()) * g.height());
    if (!combine) ok.assign(v.size(), 1);
    for (int y = 0; y < g.height(); ++y) {
        const auto* r = reinterpret_cast<const quint16*>(g.constScanLine(y));
        for (int x = 0; x < g.width(); ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * g.width() + x;
            v[i] = r[x] / 100.0f;
            if (r[x] < 15000 || r[x] > 35000) ok[i] = 0;
        }
    }
    return v;
}

} // namespace

QImage daytimeCloudMaskFullDisk(const QImage& ir1Tbb01K, const QImage& ir2Tbb01K,
                                const QImage& ir3Tbb01K, const QImage& ir4Tbb01K,
                                const QImage& visible, const ScanNavigation& navigation) {
    if (!navigation.valid || ir1Tbb01K.isNull() || ir2Tbb01K.isNull() || ir3Tbb01K.isNull() ||
        ir4Tbb01K.isNull() || visible.isNull()) return {};
    const int w = ir1Tbb01K.width(), h = ir1Tbb01K.height();
    if (ir2Tbb01K.size() != ir1Tbb01K.size() || ir3Tbb01K.size() != ir1Tbb01K.size() ||
        ir4Tbb01K.size() != ir1Tbb01K.size() || visible.width() % w != 0 || visible.height() % h != 0) return {};
    std::vector<char> ok;
    const std::vector<float> t11 = tbbVector(ir1Tbb01K, ok, false);
    const std::vector<float> t12 = tbbVector(ir2Tbb01K, ok, true);
    const std::vector<float> t67 = tbbVector(ir3Tbb01K, ok, true);
    const std::vector<float> t37 = tbbVector(ir4Tbb01K, ok, true);
    std::vector<float> m11, m12, m67, m37, sd11, dummy, max11, max25;
    windowStats(t11, ok, w, h, 1, m11, nullptr);
    windowStats(t12, ok, w, h, 1, m12, nullptr);
    windowStats(t67, ok, w, h, 1, m67, nullptr);
    windowStats(t37, ok, w, h, 1, m37, nullptr);
    windowStats(t11, ok, w, h, 2, dummy, &sd11);
    windowMax(t11, w, h, 5, max11);
    windowMax(t11, w, h, 12, max25);
    // Visible reflectance (%) on the infrared grid, its 3x3 mean and standard deviation.
    const int sx = visible.width() / w, sy = visible.height() / h;
    const QImage vimg = visible.format() == QImage::Format_Grayscale16 ? visible
                                                                       : visible.convertToFormat(QImage::Format_Grayscale8);
    std::vector<float> visRaw(static_cast<std::size_t>(w) * h), visMean, visSd;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        double sum = 0.0;
        for (int j = 0; j < sy; ++j) for (int i = 0; i < sx; ++i)
            sum += nadirStationVisibleReflectance[static_cast<std::size_t>(visibleDn(vimg, x * sx + i, y * sy + j))];
        visRaw[static_cast<std::size_t>(y) * w + x] = static_cast<float>(sum / (sx * sy));
    }
    const std::vector<char> allOk(visRaw.size(), 1);
    windowStats(visRaw, allOk, w, h, 1, visMean, &visSd);
    // Land/sea from the bundled global mask (equirectangular, land bright).
    const QImage landMap = QImage(QCoreApplication::applicationDirPath() +
                                  QStringLiteral("/resources/maps/landmask.jpg")).convertToFormat(QImage::Format_Grayscale8);
    QImage out(w, h, QImage::Format_Grayscale8);
    out.fill(255);
    constexpr double ea = 6378.137, eb = 6356.752314, e2 = 1.0 - (eb * eb) / (ea * ea);
    for (int y = 0; y < h; ++y) {
        uchar* row = out.scanLine(y);
        const double line = navigation.scanLineMin + y;
        const nadirstation::orbit::SatellitePosition sat = navigationSatelliteAtLine(navigation, line);
        const QDateTime when = navigationLineTime(navigation, line);
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            if (!ok[i] || !std::isfinite(m11[i]) || !std::isfinite(m12[i]) || !std::isfinite(m67[i]) ||
                !std::isfinite(m37[i]) || !std::isfinite(sd11[i]) || !std::isfinite(visMean[i])) continue;
            double latDeg = 0.0, lonDeg = 0.0;
            if (!navigationPixelToLatLon(navigation, y, x, latDeg, lonDeg)) continue;
            const double la = latDeg * kPi / 180.0, lo = lonDeg * kPi / 180.0;
            const double cosSun = solarZenithCosine(la, lo, when);
            if (cosSun <= std::cos(80.0 * kPi / 180.0)) continue;
            const double sl = std::sin(la), cl = std::cos(la);
            const double nn = ea / std::sqrt(1.0 - e2 * sl * sl);
            const double gx = nn * cl * std::cos(lo), gy = nn * cl * std::sin(lo), gz = nn * (1.0 - e2) * sl;
            const double vx = sat.xKm - gx, vy = sat.yKm - gy, vz = sat.zKm - gz;
            const double cosSat = (vx * cl * std::cos(lo) + vy * cl * std::sin(lo) + vz * sl) /
                                  std::sqrt(vx * vx + vy * vy + vz * vz);
            if (cosSat <= std::cos(70.0 * kPi / 180.0)) continue;
            double land = 0.0;
            if (!landMap.isNull()) {
                const int my = std::clamp(static_cast<int>((90.0 - latDeg) / 180.0 * landMap.height()), 0, landMap.height() - 1);
                double lonWrapped = std::fmod(lonDeg + 180.0, 360.0); if (lonWrapped < 0) lonWrapped += 360.0;
                const int mx = std::clamp(static_cast<int>(lonWrapped / 360.0 * landMap.width()), 0, landMap.width() - 1);
                land = landMap.constScanLine(my)[mx] > 127 ? 1.0 : 0.0;
            }
            CloudTreeFeatures f{};
            f.T11 = m11[i]; f.D = m11[i] - m12[i]; f.W = m67[i] - m11[i]; f.M = m37[i] - m11[i];
            f.SD11 = sd11[i]; f.DMAX11 = max11[i] - m11[i]; f.DMAX25 = max25[i] - m11[i];
            f.VISN = visMean[i] / std::max(cosSun, 0.17); f.VSD = visSd[i];
            f.land = land; f.cos_sat = cosSat; f.cos_sun = cosSun;
            row[x] = static_cast<uchar>(fy2gDaytimeCloudTree(f));
        }
    }
    return out;
}

ScanGeometry navigateObservation(const QString& noradId, const QString& observationDirectory,
                                 QString* report) {
    ScanGeometry geometry = scanGeometryFor(noradId);
    auto fail = [&](const QString& why) {
        if (report) *report = QStringLiteral("spin-scan navigation unavailable (%1); legacy lattice used").arg(why);
        return geometry;
    };
    const QDir directory(observationDirectory);
    const QString manifestPath = findObservationManifest(directory);
    QFile manifestFile(manifestPath);
    if (manifestPath.isEmpty() || !manifestFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return fail(QStringLiteral("no observation manifest"));
    const QJsonObject manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
    QDateTime created = QDateTime::fromString(manifest.value(QStringLiteral("created_utc")).toString(),
                                              Qt::ISODateWithMs);
    if (!created.isValid())
        created = QDateTime::fromString(manifest.value(QStringLiteral("created_utc")).toString(), Qt::ISODate);
    if (!created.isValid()) return fail(QStringLiteral("manifest has no created_utc"));
    created = created.toUTC();

    // Navigation constants (per spacecraft) from the calibration resource.
    double processingDelayS = 30.0, linePeriodS = 0.6, limbThresholdK = 190.0;
    double aspectMin = 0.9875, aspectMax = 1.0035;
    std::array<double, 3> residualRow{{0.0, 0.0, 0.0}}, residualCol{{0.0, 0.0, 0.0}};
    bool residualFromResource = false;
    {
        QFile resource(QCoreApplication::applicationDirPath() +
                       QStringLiteral("/resources/orbits/nadirstation_scan_calibration.json"));
        if (resource.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QJsonObject nav = QJsonDocument::fromJson(resource.readAll()).object()
                .value(QStringLiteral("satellites")).toObject().value(noradId).toObject()
                .value(QStringLiteral("spin_scan_navigation")).toObject();
            if (nav.contains(QStringLiteral("enabled")) && !nav.value(QStringLiteral("enabled")).toBool(true))
                return fail(QStringLiteral("disabled in nadirstation_scan_calibration.json"));
            processingDelayS = nav.value(QStringLiteral("processing_delay_s")).toDouble(processingDelayS);
            linePeriodS = nav.value(QStringLiteral("line_period_s")).toDouble(linePeriodS);
            limbThresholdK = nav.value(QStringLiteral("limb_threshold_k")).toDouble(limbThresholdK);
            const QJsonArray aspect = nav.value(QStringLiteral("disk_aspect_ry_rx_range")).toArray();
            if (aspect.size() == 2) {
                aspectMin = aspect.at(0).toDouble(aspectMin);
                aspectMax = aspect.at(1).toDouble(aspectMax);
            }
            const QJsonArray rowArray = nav.value(QStringLiteral("residual_row_px")).toArray();
            const QJsonArray colArray = nav.value(QStringLiteral("residual_col_px")).toArray();
            if (rowArray.size() == 3 && colArray.size() == 3) {
                for (int i = 0; i < 3; ++i) {
                    residualRow[static_cast<std::size_t>(i)] = rowArray.at(i).toDouble();
                    residualCol[static_cast<std::size_t>(i)] = colArray.at(i).toDouble();
                }
                residualFromResource = true;
            }
        }
    }

    const int scanLineMin = qMax(1, manifest.value(QStringLiteral("scan_line_min")).toInt(1));
    const int scanLineMax = qMax(scanLineMin, manifest.value(QStringLiteral("scan_line_max")).toInt(2291));
    QImageReader reader(directory.filePath(QStringLiteral("CH4_IR1_TBB_0p01K.png")));
    const QImage ir1 = reader.read();
    if (ir1.isNull()) return fail(QStringLiteral("CH4_IR1_TBB_0p01K.png missing"));
    // A reception that starts after line 1 is fine (FY-2H 2026-10-11 04:31, lines 2..1234: coastline
    // offsets 1-3 px): the disk fit places the rows and time runs back from the last received line.
    if (ir1.width() != static_cast<int>(kNavLattice) || ir1.height() != static_cast<int>(kNavLattice))
        return fail(QStringLiteral("infrared raster is not on the 2291x2291 lattice"));
    const DiskGeometry disk = measureDiskGeometry(ir1, limbThresholdK);
    if (!disk.valid) return fail(QStringLiteral("limb fit failed (rms %1 px, %2 points)")
                                     .arg(disk.rmsPx, 0, 'f', 2).arg(disk.points));
    // Lines lost inside a reception are not left as gaps (lines are numbered in reception order),
    // so the disk is squeezed vertically and both row placement and line timing drift across the
    // image. Complete FY-2G disks give ry/rx 0.9942..0.9970; an FY-2H scene with ~2 % lost lines
    // gave 0.978 and coastline errors growing to 28 px. Such scenes keep the legacy model.
    const double aspect = disk.ry / disk.rx;
    if (aspect < aspectMin || aspect > aspectMax)
        return fail(QStringLiteral("disk aspect ry/rx %1 outside %2..%3: lines lost inside the reception, "
                                   "row placement and line timing unreliable")
                        .arg(aspect, 0, 'f', 4).arg(aspectMin, 0, 'f', 4).arg(aspectMax, 0, 'f', 4));

    nadirstation::orbit::TleRecord tle;
    QString error;
    if (!nadirstation::orbit::loadTle(noradId, tle, &error)) return fail(error);
    // S-VISSR lines are 0.6 s apart; the manifest is written a few seconds after the last line,
    // and the recording may have started well before line 1, so time runs back from the end.
    const QDateTime scanEnd = created.addMSecs(-qRound64(processingDelayS * 1000.0));
    auto lineTime = [&](double line) {
        return scanEnd.addMSecs(-qRound64((scanLineMax - line) * linePeriodS * 1000.0));
    };
    ScanNavigation navigation;
    constexpr int knotCount = 9;
    navigation.knotFirstLine = scanLineMin;
    navigation.knotLineStep = static_cast<double>(scanLineMax - scanLineMin) / (knotCount - 1);
    for (int k = 0; k < knotCount; ++k) {
        nadirstation::orbit::SatellitePosition position;
        const double line = navigation.knotFirstLine + k * navigation.knotLineStep;
        if (!nadirstation::orbit::propagate(tle, lineTime(line), position, &error) || !position.hasOrbitNormal)
            return fail(QStringLiteral("TLE propagation failed: %1").arg(error));
        navigation.knots.push_back(position);
    }
    navigation.disk = disk;
    navigation.scanLineMin = scanLineMin;
    navigation.scanLineMax = scanLineMax;
    navigation.residualRow = residualRow;
    navigation.residualCol = residualCol;
    navigation.scanStartUtc = lineTime(scanLineMin);
    navigation.scanEndUtc = scanEnd;
    navigation.linePeriodS = linePeriodS;
    navigation.valid = true;
    const auto& mid = navigation.knots[knotCount / 2];
    navigation.summary = QStringLiteral(
        "spin-scan navigation: TLE per line (%1 .. %2 UTC), spin axis = orbit normal, "
        "sub-satellite %3 E %4 N at mid-scan; IR1 limb %5 K: centre (%6, %7) px, semi-axes (%8, %9) px, "
        "shear %10 px/line, rms %11 px, %12 points; coastline residual %13")
        .arg(navigation.scanStartUtc.toString(Qt::ISODate), scanEnd.toString(Qt::ISODate))
        .arg(mid.subpointLongitudeDeg, 0, 'f', 3).arg(mid.subpointLatitudeDeg, 0, 'f', 3)
        .arg(limbThresholdK, 0, 'f', 0)
        .arg(disk.cx, 0, 'f', 2).arg(disk.cy, 0, 'f', 2).arg(disk.rx, 0, 'f', 2).arg(disk.ry, 0, 'f', 2)
        .arg(disk.shear, 0, 'f', 5).arg(disk.rmsPx, 0, 'f', 2).arg(disk.points)
        .arg(residualFromResource ? QStringLiteral("applied from nadirstation_scan_calibration.json")
                                  : QStringLiteral("NOT applied (resource has no spin_scan_navigation block)"));
    if (nadirstation::orbit::tleIsStale(mid))
        navigation.summary += QStringLiteral("; WARNING TLE is %1 days old").arg(mid.tleAgeDays, 0, 'f', 1);
    geometry.navigation = navigation;
    if (report) *report = navigation.summary;
    return geometry;
}

namespace {

// Source-raster pixel for a geographic point under per-observation navigation. The raster may be
// the infrared grid (2291) or the visible assembly (9164, four detector lines per scan line);
// infrared pixel c covers raster pixels [s*c, s*c + s) for scale s.
bool navigatedRasterPixel(const ScanNavigation& navigation, int rasterWidth, int rasterHeight,
                          int fullDiskWidth, int fullDiskHeight, int firstRasterLine,
                          double latitudeRad, double longitudeRad, int& sx, int& sy) {
    double row = 0.0, col = 0.0;
    if (!navigationLatLonToPixel(navigation, latitudeRad * 180.0 / kPi, longitudeRad * 180.0 / kPi, row, col))
        return false;
    const DiskGeometry& d = navigation.disk;
    const double u = (col - d.cx - d.shear * (row - d.cy)) / d.rx;
    const double v = (row - d.cy) / d.ry;
    if (u * u + v * v > 1.0) return false;           // beyond the measured limb
    const double scaleX = fullDiskWidth / kNavLattice;
    const double scaleY = fullDiskHeight / kNavLattice;
    sx = static_cast<int>(std::floor((col + 0.5) * scaleX));
    const int fullRow = static_cast<int>(std::floor((row + 0.5) * scaleY));
    sy = fullRow - (qMax(1, firstRasterLine) - 1);
    return sx >= 0 && sy >= 0 && sx < rasterWidth && sy < rasterHeight;
}

} // namespace

QImage chinaGeostationaryReflectanceFactor(const QImage& visible, double centerLatitudeDeg,
                                           double centerLongitudeDeg,
                                           double subSatelliteLongitudeDeg) {    return chinaGeostationaryReflectanceFactor(visible, centerLatitudeDeg, centerLongitudeDeg,
        nadirstation::orbit::nominalGeostationary(subSatelliteLongitudeDeg), 1);
}

QImage chinaGeostationaryReflectanceFactor(const QImage& visible, double centerLatitudeDeg,
                                           double centerLongitudeDeg,
                                           const nadirstation::orbit::SatellitePosition& satellite,
                                           int sourceFirstLine,
                                           const ScanGeometry& scan,
                                           const QDateTime& observationUtc,
                                           int projectedOffsetX,
                                           int projectedOffsetY) {
    if (visible.isNull()) return {};
    if (!satellite.valid || satellite.distanceKm <= 10000.0) return {};
    constexpr double aKm = 6378.137;
    constexpr double bKm = 6356.752314;
    // The regional window spans 68 degrees longitude by 42 degrees latitude.
    // 1360x840 keeps both axes at exactly 20 pixels per degree (equirectangular).
    constexpr int mapWidth = 1360;
    constexpr int mapHeight = 840;
    constexpr double lonHalfSpan = 34.0;
    constexpr double latHalfSpan = 21.0;
    constexpr double pi = 3.14159265358979323846;
    constexpr quint16 noData = 65535;

    const double westDeg = std::clamp(centerLongitudeDeg - lonHalfSpan, -180.0, 180.0);
    const double eastDeg = std::clamp(centerLongitudeDeg + lonHalfSpan, -180.0, 180.0);
    const double southDeg = std::clamp(centerLatitudeDeg - latHalfSpan, -90.0, 90.0);
    const double northDeg = std::clamp(centerLatitudeDeg + latHalfSpan, -90.0, 90.0);
    const double eccentricity2 = 1.0 - (bKm * bKm) / (aKm * aKm);
    // The nominal lattice spans asin(R/a) over the assembled raster width; the
    // calibrated step scale changes how many source pixels that span covers.
    const double halfX = nominalHalfAngle(aKm) / scan.stepScale;
    const double halfY = nominalHalfAngle(bKm) / scan.stepScale;
    const double satLon = std::atan2(satellite.yKm, satellite.xKm);
    const double eastX = -std::sin(satLon), eastY = std::cos(satLon);
    const double upX = satellite.xKm / satellite.distanceKm;
    const double upY = satellite.yKm / satellite.distanceKm;
    const double upZ = satellite.zKm / satellite.distanceKm;
    double northX = -upZ * eastY;
    double northY = upZ * eastX;
    double northZ = upX * eastY - upY * eastX;
    const double northNorm = std::hypot(std::hypot(northX, northY), northZ);
    northX /= northNorm; northY /= northNorm; northZ /= northNorm;
    const QImage gray = visible.convertToFormat(QImage::Format_Grayscale8);
    const FY2DiskFraming framing = fy2DiskFraming(gray, sourceFirstLine);
    const int fullDiskSize = framing.fullDiskSize;
    const int fullDiskHeight = framing.scanPositions;
    // scan.residual*Px is measured in infrared-grid pixels; express it in this raster's pixels.
    const double residualScale = static_cast<double>(framing.fullDiskSize) / kResidualLatticeSize;
    QImage out(mapWidth, mapHeight, QImage::Format_Grayscale16);
    out.fill(noData);

    for (int y = 0; y < mapHeight; ++y) {
        auto* dst = reinterpret_cast<quint16*>(out.scanLine(y));
        // Pixel-centre convention and projected-map translation identical to
        // chinaGeostationaryProjection / chinaGeostationaryDataProjection, so the visible
        // product lies on exactly the same grid as every IR, CSR and cloud product.
        // (It used pixel edges and no translation before, i.e. half a pixel off.)
        const double latitude = (northDeg - (y + 0.5) * (northDeg - southDeg) / mapHeight +
                                 projectedOffsetY * (northDeg - southDeg) / mapHeight) * pi / 180.0;
        const double sinLat = std::sin(latitude), cosLat = std::cos(latitude);
        const double n = aKm / std::sqrt(1.0 - eccentricity2 * sinLat * sinLat);
        for (int x = 0; x < mapWidth; ++x) {
            const double longitude = (westDeg + (x + 0.5) * (eastDeg - westDeg) / mapWidth -
                                      projectedOffsetX * (eastDeg - westDeg) / mapWidth) * pi / 180.0;
            const double sinLon = std::sin(longitude), cosLon = std::cos(longitude);
            if (observationUtc.isValid() &&
                solarZenithCosine(latitude, longitude, observationUtc) <= std::cos(85.0 * pi / 180.0))
                continue;
            int sx = 0, sy = 0;
            if (scan.navigation.valid) {
                if (!navigatedRasterPixel(scan.navigation, gray.width(), gray.height(), fullDiskSize,
                                          fullDiskHeight, sourceFirstLine, latitude, longitude, sx, sy))
                    continue;
            } else {
            const double earthX = n * cosLat * cosLon;
            const double earthY = n * cosLat * sinLon;
            const double earthZ = n * (1.0 - eccentricity2) * sinLat;
            const double satToPointX = satellite.xKm - earthX;
            const double satToPointY = satellite.yKm - earthY;
            const double satToPointZ = satellite.zKm - earthZ;
            const double surfaceDot = satToPointX * cosLat * cosLon +
                satToPointY * cosLat * sinLon + satToPointZ * sinLat;
            if (surfaceDot <= 0.0) continue;
            const double towardX = -satToPointX, towardY = -satToPointY, towardZ = -satToPointZ;
            const double forward = -(towardX * upX + towardY * upY + towardZ * upZ);
            if (forward <= 0.0) continue;
            const double eastAngle = towardX * eastX + towardY * eastY;
            const double scanX = std::atan2(eastAngle, forward);
            const double scanY = std::atan2(towardX * northX + towardY * northY + towardZ * northZ,
                                             std::hypot(forward, eastAngle));
            sx = static_cast<int>(std::lround((scanX / halfX + 1.0) * 0.5 * (fullDiskSize - 1) - scan.residualColPx * residualScale));
            const int fullSy = static_cast<int>(std::lround((1.0 - scanY / halfY) * 0.5 * (fullDiskHeight - 1) - scan.residualRowPx * residualScale));
            sy = fullSy - qMax(1, sourceFirstLine) + 1;
            if (sx < 0 || sy < 0 || sx >= gray.width() || sy >= gray.height()) continue;
            }
            const int dn = visibleDn(gray, sx, sy);
            dst[x] = static_cast<quint16>(qRound(nadirStationVisibleReflectance[static_cast<std::size_t>(dn)] * 100.0));
        }
    }
    return out;
}

QImage chinaGeostationaryProjection(const QImage& source, double centerLatitudeDeg,
                                   double centerLongitudeDeg, double subSatelliteLongitudeDeg) {
    return chinaGeostationaryProjection(source, centerLatitudeDeg, centerLongitudeDeg,
        nadirstation::orbit::nominalGeostationary(subSatelliteLongitudeDeg), 1);
}

QImage chinaGeostationaryProjection(const QImage& source, double centerLatitudeDeg,
                                   double centerLongitudeDeg,
                                   const nadirstation::orbit::SatellitePosition& satellite,
                                   int sourceFirstLine,
                                   int projectedOffsetX,
                                   int projectedOffsetY,
                                   const ScanGeometry& scan) {
    if (source.isNull()) return {};
    if (!satellite.valid || satellite.distanceKm <= 10000.0) return {};
    constexpr double aKm = 6378.137;
    constexpr double bKm = 6356.752314;
    // Match the VIS/MCIR grid: 20 pixels per degree in both directions.
    constexpr int mapWidth = 1360;
    constexpr int mapHeight = 840;
    constexpr int outWidth = mapWidth;
    constexpr int outHeight = mapHeight;
    constexpr int mapLeft = 0;
    constexpr int mapTop = 0;
    // A China-wide view must include Taiwan and the southern Himalayan border
    // region. Keep a little margin on every side so coastlines are not clipped.
    constexpr double lonHalfSpan = 34.0;
    constexpr double latHalfSpan = 21.0;
    const double westDeg = std::clamp(centerLongitudeDeg - lonHalfSpan, -180.0, 180.0);
    const double eastDeg = std::clamp(centerLongitudeDeg + lonHalfSpan, -180.0, 180.0);
    const double southDeg = std::clamp(centerLatitudeDeg - latHalfSpan, -90.0, 90.0);
    const double northDeg = std::clamp(centerLatitudeDeg + latHalfSpan, -90.0, 90.0);
    constexpr double pi = 3.14159265358979323846;
    const double eccentricity2 = 1.0 - (bKm * bKm) / (aKm * aKm);
    const double halfX = nominalHalfAngle(aKm) / scan.stepScale;
    const double halfY = nominalHalfAngle(bKm) / scan.stepScale;
    const double satLon = std::atan2(satellite.yKm, satellite.xKm);
    const double eastX = -std::sin(satLon), eastY = std::cos(satLon);
    const double upX = satellite.xKm / satellite.distanceKm;
    const double upY = satellite.yKm / satellite.distanceKm;
    const double upZ = satellite.zKm / satellite.distanceKm;
    double northX = upY * 0.0 - upZ * eastY;
    double northY = upZ * eastX - upX * 0.0;
    double northZ = upX * eastY - upY * eastX;
    const double northNorm = std::hypot(std::hypot(northX, northY), northZ);
    northX /= northNorm; northY /= northNorm; northZ /= northNorm;
    const bool preserveColor = !source.isGrayscale();
    const QImage rgb = source.convertToFormat(QImage::Format_RGB32);
    const QImage gray = source.convertToFormat(QImage::Format_Grayscale8);
    QImage out(outWidth, outHeight, QImage::Format_RGB32);
    out.fill(qRgb(4, 12, 24));
    // Preserve dark-but-valid pixels instead of crushing the bottom 2% to
    // the same black shade. Invalid zero fill is handled separately below.
    const auto range = robustRange(gray, nullptr, nullptr, nullptr, 0.0, 1.0);
    // S-VISSR's infrared full-disk grid is 2,291 x 2,291; the visible assembly is
    // four times taller because it stacks the four visible detector lines of each
    // scan position. Partial products contain the first N scan lines, not a
    // vertically rescaled complete disk.
    const FY2DiskFraming framing = fy2DiskFraming(gray, sourceFirstLine);
    const int fullDiskSize = framing.fullDiskSize;
    const int fullDiskHeight = framing.scanPositions;
    // scan.residual*Px is measured in infrared-grid pixels; express it in this raster's pixels.
    const double residualScale = static_cast<double>(framing.fullDiskSize) / kResidualLatticeSize;

    for (int y = 0; y < mapHeight; ++y) {
        auto* dst = reinterpret_cast<QRgb*>(out.scanLine(mapTop + y)) + mapLeft;
        const double latitude = (northDeg - (y + 0.5) * (northDeg - southDeg) / mapHeight +
                                 projectedOffsetY * (northDeg - southDeg) / mapHeight) * pi / 180.0;
        const double sinLat = std::sin(latitude), cosLat = std::cos(latitude);
        const double n = aKm / std::sqrt(1.0 - eccentricity2 * sinLat * sinLat);
        for (int x = 0; x < mapWidth; ++x) {
            const double longitude = (westDeg + (x + 0.5) * (eastDeg - westDeg) / mapWidth -
                                      projectedOffsetX * (eastDeg - westDeg) / mapWidth) * pi / 180.0;
            const double sinLon = std::sin(longitude), cosLon = std::cos(longitude);
            int sx = 0, sy = 0;
            if (scan.navigation.valid) {
                // navigatedRasterPixel already rejects points beyond the measured limb.
                if (!navigatedRasterPixel(scan.navigation, gray.width(), gray.height(), fullDiskSize,
                                          fullDiskHeight, sourceFirstLine, latitude, longitude, sx, sy))
                    continue;
            } else {
            const double earthX = n * cosLat * cosLon;
            const double earthY = n * cosLat * sinLon;
            const double earthZ = n * (1.0 - eccentricity2) * sinLat;
            const double satToPointX = satellite.xKm - earthX;
            const double satToPointY = satellite.yKm - earthY;
            const double satToPointZ = satellite.zKm - earthZ;
            const double surfaceDot = satToPointX * cosLat * cosLon +
                satToPointY * cosLat * sinLon + satToPointZ * sinLat;
            if (surfaceDot <= 0.0) continue;
            const double towardX = -satToPointX, towardY = -satToPointY, towardZ = -satToPointZ;
            const double forward = -(towardX * upX + towardY * upY + towardZ * upZ);
            if (forward <= 0.0) continue;
            const double scanX = std::atan2(towardX * eastX + towardY * eastY, forward);
            const double scanY = std::atan2(towardX * northX + towardY * northY + towardZ * northZ,
                                             std::hypot(forward, towardX * eastX + towardY * eastY));
            sx = static_cast<int>(std::lround((scanX / halfX + 1.0) * 0.5 * (fullDiskSize - 1) - scan.residualColPx * residualScale));
            const int fullSy = static_cast<int>(std::lround((1.0 - scanY / halfY) * 0.5 * (fullDiskHeight - 1) - scan.residualRowPx * residualScale));
            sy = fullSy - qMax(1, sourceFirstLine) + 1;
            if (sx < 0 || sy < 0 || sx >= gray.width() || sy >= gray.height()) continue;
            // The decoded disk PNG is stored in a square and its corners are
            // white fill, not radiance. A geostationary forward projection can
            // map a valid Earth point into those corner pixels near the limb;
            // treating them as samples creates rectangular seams in MCIR.
            // Mask the square's outside using the disk ellipse measured from
            // the S-VISSR full-disk framing (slightly oblate in pixel space).
            // The disk is centred on the scan origin, which the calibrated residual moves away from
            // the middle of the raster, so the mask is moved with it.
            const double diskCx = (gray.width() - 1) * 0.5 - scan.residualColPx * residualScale;
            const double diskCy = (gray.height() - 1) * 0.5 - scan.residualRowPx * residualScale;
            const double diskRx = gray.width() * 0.478;
            const double diskRy = gray.height() * 0.473;
            const double diskX = (sx - diskCx) / diskRx;
            const double diskY = (sy - diskCy) / diskRy;
            if (diskX * diskX + diskY * diskY > 1.0) continue;
            }
            if (preserveColor) {
                // False Color and other RGB inputs are display products already.
                // Reproject their RGB values directly; grayscale stretching here
                // would discard the channel-composite colors.
                dst[x] = rgb.pixel(sx, sy);
                continue;
            }
            int value = gray.constScanLine(sy)[sx];
            // Saturated/high-end counts are valid measurements; use them in
            // rendering even though robustRange omits them when estimating
            // display contrast. Repair isolated zero-fill or impulse speckles
            // only when their 5x5 neighborhood strongly agrees; larger holes and
            // coherent weather features are left untouched.
            if (value == 0) {
                // Zero-fill speckles are common in the VIS assembly. Repair them
                // only for this projected display, and only when nearby valid
                // samples form a strong local consensus. Larger coherent gaps
                // remain transparent/background; calibrated source products are
                // never edited by this interpolation.
                for (const int radius : {3, 5}) {
                    std::array<int, 120> neighbors{};
                    int n = 0;
                    if (sx < radius || sy < radius || sx + radius >= gray.width() || sy + radius >= gray.height()) continue;
                    for (int dy = -radius; dy <= radius; ++dy) for (int dx = -radius; dx <= radius; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        const int v = gray.constScanLine(sy + dy)[sx + dx];
                        if (v > 0) neighbors[static_cast<std::size_t>(n++)] = v;
                    }
                    const int windowSamples = (2 * radius + 1) * (2 * radius + 1) - 1;
                    if (n * 10 < windowSamples * 7) continue;
                    std::sort(neighbors.begin(), neighbors.begin() + n);
                    const int median = neighbors[static_cast<std::size_t>(n / 2)];
                    value = median;
                    break;
                }
                if (value == 0) continue;
            } else {
                std::array<int, 24> neighbors{};
                int n = 0;
                if (sx >= 2 && sy >= 2 && sx + 2 < gray.width() && sy + 2 < gray.height()) {
                    for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        const int v = gray.constScanLine(sy + dy)[sx + dx];
                        if (v > 0) neighbors[static_cast<std::size_t>(n++)] = v;
                    }
                }
                if (n >= 15) {
                    std::sort(neighbors.begin(), neighbors.begin() + n);
                    const int median = neighbors[static_cast<std::size_t>(n / 2)];
                    int consistent = 0;
                    for (int i = 0; i < n; ++i)
                        if (std::abs(neighbors[static_cast<std::size_t>(i)] - median) <= 28) ++consistent;
                    if (consistent >= 15 && std::abs(value - median) >= 60) value = median;
                }
            }
            const int stretched = stretch8(value, range);
            const int ink = 40 + qRound(stretched * 0.78);
            dst[x] = qRgb(ink, ink, std::min(255, ink + 4));
        }
    }
    // This is a satellite-only reprojection. Do not overlay borders or graticules.
    if (!preserveColor) fillSmallProjectedGaps(out);
    return out;
}

QImage chinaGeostationaryDataProjection(const QImage& source, double centerLatitudeDeg,
                                        double centerLongitudeDeg,
                                        const nadirstation::orbit::SatellitePosition& satellite,
                                        int sourceFirstLine,
                                        int projectedOffsetX,
                                        int projectedOffsetY,
                                        const ScanGeometry& scan) {
    if (source.isNull() || !satellite.valid || satellite.distanceKm <= 10000.0) return {};
    constexpr double aKm = 6378.137;
    constexpr double bKm = 6356.752314;
    constexpr double pi = 3.14159265358979323846;
    constexpr int mapWidth = 1360, mapHeight = 840;
    constexpr double lonHalfSpan = 34.0, latHalfSpan = 21.0;
    const double westDeg = std::clamp(centerLongitudeDeg - lonHalfSpan, -180.0, 180.0);
    const double eastDeg = std::clamp(centerLongitudeDeg + lonHalfSpan, -180.0, 180.0);
    const double southDeg = std::clamp(centerLatitudeDeg - latHalfSpan, -90.0, 90.0);
    const double northDeg = std::clamp(centerLatitudeDeg + latHalfSpan, -90.0, 90.0);
    const double eccentricity2 = 1.0 - (bKm * bKm) / (aKm * aKm);
    const double halfX = nominalHalfAngle(aKm) / scan.stepScale;
    const double halfY = nominalHalfAngle(bKm) / scan.stepScale;
    const double satLon = std::atan2(satellite.yKm, satellite.xKm);
    const double eastX = -std::sin(satLon), eastY = std::cos(satLon);
    const double upX = satellite.xKm / satellite.distanceKm;
    const double upY = satellite.yKm / satellite.distanceKm;
    const double upZ = satellite.zKm / satellite.distanceKm;
    double northX = -upZ * eastY;
    double northY = upZ * eastX;
    double northZ = upX * eastY - upY * eastX;
    const double northNorm = std::hypot(std::hypot(northX, northY), northZ);
    northX /= northNorm; northY /= northNorm; northZ /= northNorm;

    enum class PixelKind { Gray8, Gray16, Rgb };
    PixelKind kind;
    QImage input;
    if (source.format() == QImage::Format_Grayscale16) {
        kind = PixelKind::Gray16;
        input = source;
    } else if (source.isGrayscale()) {
        kind = PixelKind::Gray8;
        input = source.convertToFormat(QImage::Format_Grayscale8);
    } else {
        kind = PixelKind::Rgb;
        input = source.convertToFormat(QImage::Format_RGB32);
    }
    const QImage::Format format = kind == PixelKind::Gray16 ? QImage::Format_Grayscale16
        : kind == PixelKind::Gray8 ? QImage::Format_Grayscale8 : QImage::Format_RGB32;
    QImage out(mapWidth, mapHeight, format);
    out.fill(0);
    const FY2DiskFraming framing = fy2DiskFraming(input, sourceFirstLine);
    const int fullDiskSize = framing.fullDiskSize;
    const int fullDiskHeight = framing.scanPositions;
    // scan.residual*Px is measured in infrared-grid pixels; express it in this raster's pixels.
    const double residualScale = static_cast<double>(framing.fullDiskSize) / kResidualLatticeSize;
    const int firstLine = qMax(1, sourceFirstLine);
    for (int y = 0; y < mapHeight; ++y) {
        const double latitude = (northDeg - (y + 0.5) * (northDeg - southDeg) / mapHeight +
                                 projectedOffsetY * (northDeg - southDeg) / mapHeight) * pi / 180.0;
        const double sinLat = std::sin(latitude), cosLat = std::cos(latitude);
        const double n = aKm / std::sqrt(1.0 - eccentricity2 * sinLat * sinLat);
        auto* dst8 = kind == PixelKind::Gray8 ? out.scanLine(y) : nullptr;
        auto* dst16 = kind == PixelKind::Gray16 ? reinterpret_cast<quint16*>(out.scanLine(y)) : nullptr;
        auto* dstRgb = kind == PixelKind::Rgb ? reinterpret_cast<QRgb*>(out.scanLine(y)) : nullptr;
        for (int x = 0; x < mapWidth; ++x) {
            const double longitude = (westDeg + (x + 0.5) * (eastDeg - westDeg) / mapWidth -
                                      projectedOffsetX * (eastDeg - westDeg) / mapWidth) * pi / 180.0;
            const double sinLon = std::sin(longitude), cosLon = std::cos(longitude);
            int sx = 0, sy = 0;
            if (scan.navigation.valid) {
                if (!navigatedRasterPixel(scan.navigation, input.width(), input.height(), fullDiskSize,
                                          fullDiskHeight, firstLine, latitude, longitude, sx, sy))
                    continue;
            } else {
            const double earthX = n * cosLat * cosLon;
            const double earthY = n * cosLat * sinLon;
            const double earthZ = n * (1.0 - eccentricity2) * sinLat;
            const double satToPointX = satellite.xKm - earthX;
            const double satToPointY = satellite.yKm - earthY;
            const double satToPointZ = satellite.zKm - earthZ;
            const double surfaceDot = satToPointX * cosLat * cosLon +
                satToPointY * cosLat * sinLon + satToPointZ * sinLat;
            if (surfaceDot <= 0.0) continue;
            const double towardX = -satToPointX, towardY = -satToPointY, towardZ = -satToPointZ;
            const double forward = -(towardX * upX + towardY * upY + towardZ * upZ);
            if (forward <= 0.0) continue;
            const double eastAngle = towardX * eastX + towardY * eastY;
            const double scanX = std::atan2(eastAngle, forward);
            const double scanY = std::atan2(towardX * northX + towardY * northY + towardZ * northZ,
                                             std::hypot(forward, eastAngle));
            sx = static_cast<int>(std::lround((scanX / halfX + 1.0) * 0.5 * (fullDiskSize - 1) - scan.residualColPx * residualScale));
            const int fullSy = static_cast<int>(std::lround((1.0 - scanY / halfY) * 0.5 * (fullDiskHeight - 1) - scan.residualRowPx * residualScale));
            sy = fullSy - firstLine + 1;
            if (sx < 0 || sy < 0 || sx >= input.width() || sy >= input.height()) continue;
            }
            if (kind == PixelKind::Gray8) dst8[x] = input.constScanLine(sy)[sx];
            else if (kind == PixelKind::Gray16)
                dst16[x] = reinterpret_cast<const quint16*>(input.constScanLine(sy))[sx];
            else dstRgb[x] = reinterpret_cast<const QRgb*>(input.constScanLine(sy))[sx];
        }
    }
    return out;
}

QImage chinaGeostationaryMcirProjection(const QImage& ir1, double centerLatitudeDeg,
                                        double centerLongitudeDeg,
                                        double subSatelliteLongitudeDeg,
                                        bool sourceIsBrightnessTemperature) {
    return chinaGeostationaryMcirProjection(ir1, centerLatitudeDeg, centerLongitudeDeg,
        nadirstation::orbit::nominalGeostationary(subSatelliteLongitudeDeg), sourceIsBrightnessTemperature, 1, 0, 0);
}

QImage chinaGeostationaryMcirProjection(const QImage& ir1, double centerLatitudeDeg,
                                        double centerLongitudeDeg,
                                        const nadirstation::orbit::SatellitePosition& satellite,
                                        bool sourceIsBrightnessTemperature,
                                        int sourceFirstLine,
                                        int projectedLayerOffsetX,
                                        int projectedLayerOffsetY) {
    if (ir1.isNull()) return {};
    if (!satellite.valid) return {};
    // MCIR here means a geographic basemap underlay with the received IR1
    // cloud signal blended over it. The basemap follows the output lon/lat grid;
    // the image sampling follows the selected satellite's geostationary orbit.
    QImage projectionSignal = ir1;
    if (sourceIsBrightnessTemperature) {
        const QImage tbb = ir1.convertToFormat(QImage::Format_Grayscale16);
        projectionSignal = QImage(tbb.size(), QImage::Format_Grayscale8);
        for (int y = 0; y < tbb.height(); ++y) {
            const auto* temperatures = reinterpret_cast<const quint16*>(tbb.constScanLine(y));
            uchar* coldness = projectionSignal.scanLine(y);
            for (int x = 0; x < tbb.width(); ++x) {
                if (temperatures[x] == 0) { coldness[x] = 0; continue; }
                const double kelvin = temperatures[x] / 100.0;
                // Cold cloud tops (low TBB) become the bright MCIR layer;
                // warm land/ocean stay transparent over the geographic map.
                coldness[x] = static_cast<uchar>(qBound(1,
                    1 + qRound((300.0 - kelvin) * 2.54), 255));
            }
        }
    }
    const QImage thermal = chinaGeostationaryProjection(
        projectionSignal, centerLatitudeDeg, centerLongitudeDeg, satellite, sourceFirstLine,
        projectedLayerOffsetX, projectedLayerOffsetY);
    if (thermal.isNull()) return {};

    // 68° x 42° at 20 pixels/degree: true equirectangular output grid.
    constexpr int width = 1360, height = 840;
    constexpr double lonHalfSpan = 34.0, latHalfSpan = 21.0;
    const double west = std::clamp(centerLongitudeDeg - lonHalfSpan, -180.0, 180.0);
    const double east = std::clamp(centerLongitudeDeg + lonHalfSpan, -180.0, 180.0);
    const double south = std::clamp(centerLatitudeDeg - latHalfSpan, -90.0, 90.0);
    const double north = std::clamp(centerLatitudeDeg + latHalfSpan, -90.0, 90.0);
    constexpr double aKm = 6378.137;
    constexpr double bKm = 6356.752314;
    constexpr double pi = 3.14159265358979323846;
    constexpr double eccentricity2 = 1.0 - (bKm * bKm) / (aKm * aKm);
    
    const QString mapPath = QCoreApplication::applicationDirPath() +
        QStringLiteral("/resources/maps/world_relief.jpg");
    constexpr QSize scaledWorldSize(7200, 3600);
    const QRect clip(qBound(0, qRound((west + 180.0) / 360.0 * scaledWorldSize.width()), scaledWorldSize.width() - 1),
                     qBound(0, qRound((90.0 - north) / 180.0 * scaledWorldSize.height()), scaledWorldSize.height() - 1),
                     qMax(1, qRound((east - west) / 360.0 * scaledWorldSize.width())),
                     qMax(1, qRound((north - south) / 180.0 * scaledWorldSize.height())));
    QImageReader mapReader(mapPath);
    mapReader.setScaledSize(scaledWorldSize);
    mapReader.setScaledClipRect(clip);
    QImage map = mapReader.read();
    if (map.size() == scaledWorldSize) map = map.copy(clip);
    if (map.isNull()) {
        QImageReader clippedReader(mapPath);
        const QSize original = clippedReader.size();
        const QRect sourceClip(qBound(0, qRound((west + 180.0) / 360.0 * original.width()), original.width() - 1),
                               qBound(0, qRound((90.0 - north) / 180.0 * original.height()), original.height() - 1),
                               qMax(1, qRound((east - west) / 360.0 * original.width())),
                               qMax(1, qRound((north - south) / 180.0 * original.height())));
        clippedReader.setClipRect(sourceClip);
        clippedReader.setScaledSize(QSize(width, height));
        map = clippedReader.read();
    }
    if (map.isNull()) return {};
    if (map.size() != QSize(width, height))
        map = map.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    map = map.convertToFormat(QImage::Format_RGB32);

    // Each channel is histogram-equalized before composite generation:
    // build a per-scene CDF for this projected
    // IR1 layer, ignoring pixels outside the satellite view.
    std::array<quint64, 256> histogram{};
    quint64 validCount = 0;
    constexpr QRgb outsideDisk = qRgb(4, 12, 24);
    for (int y = 0; y < thermal.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(thermal.constScanLine(y));
        for (int x = 0; x < thermal.width(); ++x) {
            if (row[x] == outsideDisk) continue;
            ++histogram[static_cast<std::size_t>(qGray(row[x]))];
            ++validCount;
        }
    }
    std::array<uchar, 256> equalized{};
    if (validCount > 0) {
        quint64 cumulative = 0;
        quint64 first = 0;
        bool haveFirst = false;
        for (int value = 0; value < 256; ++value) {
            cumulative += histogram[static_cast<std::size_t>(value)];
            if (!haveFirst && histogram[static_cast<std::size_t>(value)] != 0) {
                first = cumulative;
                haveFirst = true;
            }
            const quint64 denominator = std::max<quint64>(1, validCount - first);
            const quint64 numerator = cumulative > first ? cumulative - first : 0;
            equalized[static_cast<std::size_t>(value)] = static_cast<uchar>(qBound(0,
                qRound(255.0 * static_cast<double>(numerator) / static_cast<double>(denominator)), 255));
        }
    }

    QImage out(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y) {
        const auto* sourceRow = reinterpret_cast<const QRgb*>(thermal.constScanLine(y));
        const auto* mapRow = reinterpret_cast<const QRgb*>(map.constScanLine(y));
        auto* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
        const double latitude = (north - y * (north - south) / height) * pi / 180.0;
        const double sinLat = std::sin(latitude), cosLat = std::cos(latitude);
        const double n = aKm / std::sqrt(1.0 - eccentricity2 * sinLat * sinLat);
        for (int x = 0; x < width; ++x) {
            const QRgb base = mapRow[x];
            const QRgb sample = sourceRow[x];
            if (sample == qRgb(4, 12, 24)) { dst[x] = base; continue; }
            const int signal = equalized[static_cast<std::size_t>(qGray(sample))];
            // Neutral grayscale IR overlay; do not introduce a yellow hue.
            // This is a per-channel histogram stretch for display, not
            // calibrated brightness temperature in kelvin.
            const double longitude = (west + x * (east - west) / width) * pi / 180.0;
            const double earthX = n * cosLat * std::cos(longitude);
            const double earthY = n * cosLat * std::sin(longitude);
            const double earthZ = n * (1.0 - eccentricity2) * sinLat;
            const double satToPointX = satellite.xKm - earthX;
            const double satToPointY = satellite.yKm - earthY;
            const double satToPointZ = satellite.zKm - earthZ;
            const double surfaceDot = satToPointX * cosLat * std::cos(longitude) +
                satToPointY * cosLat * std::sin(longitude) + satToPointZ * sinLat;
            const double distance = std::sqrt(satToPointX * satToPointX +
                satToPointY * satToPointY + satToPointZ * satToPointZ);
            const double viewCosine = distance > 0.0 ? surfaceDot / distance : 0.0;
            const double edgeT = std::clamp((viewCosine - 0.04) / 0.18, 0.0, 1.0);
            const double edgeFade = edgeT * edgeT * (3.0 - 2.0 * edgeT);
            const double amount = std::clamp((signal - 28.0) / 218.0, 0.0, 0.96) * edgeFade;
            const int cloudGray = qRound(218.0 + 37.0 * amount);
            dst[x] = qRgb(qRound(qRed(base) * (1.0 - amount) + cloudGray * amount),
                          qRound(qGreen(base) * (1.0 - amount) + cloudGray * amount),
                          qRound(qBlue(base) * (1.0 - amount) + cloudGray * amount));
        }
    }
    return out;
}

QImage irEnhancement(const QImage& ir1, const QImage& ir2) {
    // Display-only thermal enhancement. FY-2 official products should use the
    // channel-specific calibration LUTs to obtain brightness temperature first;
    // this viewer does not yet expose those LUTs, so this is intentionally a
    // false-color display transform rather than a physical-temperature product.
    QImage a = ir1.convertToFormat(QImage::Format_Grayscale8);
    QImage b = ir2.convertToFormat(QImage::Format_Grayscale8);
    if (a.size() != b.size()) b = b.scaled(a.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage out(a.size(), QImage::Format_RGB32);

    auto percentile = [](const QImage& im, double q) -> float {
        std::vector<int> sample;
        const int sx = std::max(1, im.width() / 256);
        const int sy = std::max(1, im.height() / 128);
        for (int y = 0; y < im.height(); y += sy)
            for (int x = 0; x < im.width(); x += sx)
                sample.push_back(qGray(im.pixel(x, y)));
        if (sample.empty()) return 0.0f;
        std::sort(sample.begin(), sample.end());
        const std::size_t idx = static_cast<std::size_t>(qBound(0.0, q, 1.0) * static_cast<double>(sample.size() - 1));
        return static_cast<float>(sample[idx]);
    };

    const float lo1 = percentile(a, 0.02), hi1 = std::max(lo1 + 1.0f, percentile(a, 0.98));
    const float lo2 = percentile(b, 0.02), hi2 = std::max(lo2 + 1.0f, percentile(b, 0.98));

    for (int y = 0; y < a.height(); ++y) for (int x = 0; x < a.width(); ++x) {
        const float n1 = std::clamp((qGray(a.pixel(x, y)) - lo1) / (hi1 - lo1), 0.0f, 1.0f);
        const float n2 = std::clamp((qGray(b.pixel(x, y)) - lo2) / (hi2 - lo2), 0.0f, 1.0f);
        const float cold = 1.0f - n1;
        const float split = std::clamp(0.5f + 1.4f * (n2 - n1), 0.0f, 1.0f);

        // Thermal palette: charcoal/red -> violet -> pale cyan/white for the
        // coldest cloud tops. No green-dominant plateau.
        float r, g, bl;
        if (cold < 0.35f) {
            const float u = cold / 0.35f;
            r = 36.0f + 175.0f * u;
            g = 16.0f + 22.0f * u;
            bl = 18.0f + 28.0f * u;
        } else if (cold < 0.72f) {
            const float u = (cold - 0.35f) / 0.37f;
            r = 211.0f - 88.0f * u;
            g = 38.0f + 12.0f * u;
            bl = 46.0f + 128.0f * u;
        } else {
            const float u = (cold - 0.72f) / 0.28f;
            r = 123.0f + 112.0f * u;
            g = 50.0f + 125.0f * u;
            bl = 174.0f + 71.0f * u;
        }
        r *= (0.90f + 0.15f * split);
        bl *= (1.02f - 0.10f * split);
        const float lum = 0.70f + 0.30f * std::pow(std::clamp(0.70f * cold + 0.30f * split, 0.0f, 1.0f), 0.65f);
        r *= lum; g *= lum; bl *= lum;
        out.setPixel(x, y, qRgb(qBound(0, static_cast<int>(std::lround(r)), 255),
                                qBound(0, static_cast<int>(std::lround(g)), 255),
                                qBound(0, static_cast<int>(std::lround(bl)), 255)));
    }
    return out;
}

QImage longwaveIrColorEnhancement(const QImage& brightnessTemperature01K) {
    if (brightnessTemperature01K.isNull()) return {};
    const QImage tbb = brightnessTemperature01K.convertToFormat(QImage::Format_Grayscale16);
    QImage out(tbb.size(), QImage::Format_RGB32);
    // Display palette: cold cloud tops in blue/cyan, warmer surfaces in
    // yellow/orange/red. This is a color enhancement of calibrated IR1 TBB.
    constexpr std::array<double, 8> stops{180.0, 200.0, 220.0, 240.0,
                                           260.0, 280.0, 300.0, 320.0};
    constexpr std::array<QColor, 8> colors{
        QColor(35, 55, 210), QColor(35, 145, 255), QColor(35, 220, 235),
        QColor(45, 195, 120), QColor(155, 220, 65), QColor(255, 220, 55),
        QColor(255, 135, 35), QColor(205, 35, 45)};
    for (int y = 0; y < tbb.height(); ++y) {
        const auto* source = reinterpret_cast<const quint16*>(tbb.constScanLine(y));
        auto* target = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < tbb.width(); ++x) {
            const double kelvin = source[x] / 100.0;
            // Match the decoder's documented physical-range validity gate;
            // calibration fill values such as 119.55 K stay black/no-data.
            if (kelvin < 150.0 || kelvin > 350.0) {
                target[x] = qRgb(0, 0, 0);
                continue;
            }
            const double clipped = std::clamp(kelvin, stops.front(), stops.back());
            const auto upper = std::upper_bound(stops.begin(), stops.end(), clipped);
            const std::size_t hi = upper == stops.end() ? stops.size() - 1
                : static_cast<std::size_t>(upper - stops.begin());
            const std::size_t lo = hi == 0 ? 0 : hi - 1;
            const double mix = hi == lo ? 0.0
                : (clipped - stops[lo]) / (stops[hi] - stops[lo]);
            const QColor c0 = colors[lo], c1 = colors[hi];
            target[x] = qRgb(qRound(c0.red() * (1.0 - mix) + c1.red() * mix),
                             qRound(c0.green() * (1.0 - mix) + c1.green() * mix),
                             qRound(c0.blue() * (1.0 - mix) + c1.blue() * mix));
        }
    }
    return out;
}

QImage cloudDetectionFromTbb(const QImage& input, double thresholdKelvin) {
    if (input.isNull()) return {};
    const QImage tbb = input.convertToFormat(QImage::Format_Grayscale16);
    QImage out(tbb.size(), QImage::Format_Grayscale8);
    const int threshold = qRound(thresholdKelvin * 100.0);
    for (int y = 0; y < tbb.height(); ++y) {
        const auto* src = reinterpret_cast<const quint16*>(tbb.constScanLine(y));
        auto* dst = out.scanLine(y);
        for (int x = 0; x < tbb.width(); ++x) {
            const int kelvin100 = src[x];
            dst[x] = (kelvin100 < 15000 || kelvin100 > 35000) ? 255
                : (kelvin100 <= threshold ? 1 : 0);
        }
    }
    return out;
}

namespace {
bool validTbb(quint16 value) { return value >= 15000 && value <= 35000; }

QImage indexedCloudMask(const QImage& ir1Input, const QImage& ir2Input,
                        const QImage& ir3Input, const QImage& ir4Input,
                        QImage* classification = nullptr) {
    if (ir1Input.isNull() || ir2Input.isNull() || ir3Input.isNull() || ir4Input.isNull() ||
        ir1Input.size() != ir2Input.size() || ir1Input.size() != ir3Input.size() ||
        ir1Input.size() != ir4Input.size()) return {};
    const QImage ir1 = ir1Input.convertToFormat(QImage::Format_Grayscale16);
    const QImage ir2 = ir2Input.convertToFormat(QImage::Format_Grayscale16);
    const QImage ir3 = ir3Input.convertToFormat(QImage::Format_Grayscale16);
    const QImage ir4 = ir4Input.convertToFormat(QImage::Format_Grayscale16);
    QImage mask(ir1.size(), QImage::Format_Indexed8);
    QVector<QRgb> maskColors(256, qRgba(0, 0, 0, 0));
    maskColors[0] = qRgb(30, 38, 46);
    maskColors[1] = qRgb(240, 240, 240);
    maskColors[255] = qRgb(115, 125, 138);
    mask.setColorTable(maskColors);
    if (classification) {
        *classification = QImage(ir1.size(), QImage::Format_Indexed8);
        QVector<QRgb> colors(256, qRgba(0, 0, 0, 0));
        colors[0] = qRgb(30, 38, 46);       // clear
        colors[1] = qRgb(230, 230, 230);   // cloud candidate
        colors[2] = qRgb(200, 95, 220);     // cold-cloud candidate
        colors[3] = qRgb(245, 180, 55);    // warmer-cloud candidate
        colors[4] = qRgb(70, 205, 235);    // split-window cirrus candidate
        colors[255] = qRgb(115, 125, 138); // invalid / no data
        classification->setColorTable(colors);
    }

    auto localMedian = [&](int x, int y) {
        std::array<int, 9> values{};
        int count = 0;
        for (int dy = -1; dy <= 1; ++dy) {
            const int yy = y + dy;
            if (yy < 0 || yy >= ir1.height()) continue;
            const auto* row = reinterpret_cast<const quint16*>(ir1.constScanLine(yy));
            for (int dx = -1; dx <= 1; ++dx) {
                const int xx = x + dx;
                if (xx < 0 || xx >= ir1.width() || !validTbb(row[xx])) continue;
                values[static_cast<std::size_t>(count++)] = row[xx];
            }
        }
        if (count == 0) return 0;
        std::sort(values.begin(), values.begin() + count);
        return values[static_cast<std::size_t>(count / 2)];
    };

    for (int y = 0; y < ir1.height(); ++y) {
        const auto* t1 = reinterpret_cast<const quint16*>(ir1.constScanLine(y));
        const auto* t2 = reinterpret_cast<const quint16*>(ir2.constScanLine(y));
        const auto* t3 = reinterpret_cast<const quint16*>(ir3.constScanLine(y));
        const auto* t4 = reinterpret_cast<const quint16*>(ir4.constScanLine(y));
        auto* maskRow = mask.scanLine(y);
        auto* classRow = classification ? classification->scanLine(y) : nullptr;
        for (int x = 0; x < ir1.width(); ++x) {
            if (!validTbb(t1[x]) || !validTbb(t2[x]) || !validTbb(t3[x]) || !validTbb(t4[x])) {
                maskRow[x] = 255;
                if (classRow) classRow[x] = 255;
                continue;
            }
            const int splitWindowHundredths = static_cast<int>(t1[x]) - static_cast<int>(t2[x]);
            const int neighborhoodContrast = localMedian(x, y) - static_cast<int>(t1[x]);
            const bool thinCirrus = t1[x] >= 27315 && t1[x] <= 28500 && splitWindowHundredths >= 120;
            const bool coldCloud = t1[x] <= 27315 && (t2[x] <= 27515 || t1[x] <= 25815);
            const bool localCloud = t1[x] <= 28000 && neighborhoodContrast >= 350;
            const bool cloudy = thinCirrus || coldCloud || localCloud;
            maskRow[x] = cloudy ? 1 : 0;
            if (!classRow) continue;
            if (!cloudy) classRow[x] = 0;
            else if (t1[x] <= 23500) classRow[x] = 2;
            else if (t1[x] <= 27315) classRow[x] = 3;
            else if (thinCirrus) classRow[x] = 4;
            else classRow[x] = 1;
        }
    }
    return mask;
}
}

QImage cloudDetectionFromTbb(const QImage& ir1, const QImage& ir2,
                             const QImage& ir3, const QImage& ir4) {
    return indexedCloudMask(ir1, ir2, ir3, ir4);
}

QImage cloudTopTemperatureFromTbb(const QImage& input, double cloudThresholdKelvin) {
    if (input.isNull()) return {};
    const QImage tbb = input.convertToFormat(QImage::Format_Grayscale16);
    QImage out(tbb.size(), QImage::Format_Grayscale16);
    const int threshold = qRound(cloudThresholdKelvin * 100.0);
    for (int y = 0; y < tbb.height(); ++y) {
        const auto* src = reinterpret_cast<const quint16*>(tbb.constScanLine(y));
        auto* dst = reinterpret_cast<quint16*>(out.scanLine(y));
        for (int x = 0; x < tbb.width(); ++x)
            dst[x] = (src[x] >= 15000 && src[x] <= threshold) ? src[x] : 0;
    }
    return out;
}

QImage cloudTopTemperatureFromTbb(const QImage& input, const QImage& cloudMaskInput) {
    if (input.isNull() || cloudMaskInput.isNull() || input.size() != cloudMaskInput.size()) return {};
    const QImage tbb = input.convertToFormat(QImage::Format_Grayscale16);
    const QImage mask = cloudMaskInput.format() == QImage::Format_Indexed8
        ? cloudMaskInput : cloudMaskInput.convertToFormat(QImage::Format_Grayscale8);
    QImage out(tbb.size(), QImage::Format_Grayscale16);
    for (int y = 0; y < tbb.height(); ++y) {
        const auto* src = reinterpret_cast<const quint16*>(tbb.constScanLine(y));
        const uchar* maskRow = mask.constScanLine(y);
        auto* dst = reinterpret_cast<quint16*>(out.scanLine(y));
        for (int x = 0; x < tbb.width(); ++x)
            dst[x] = (maskRow[x] == 1 && validTbb(src[x])) ? src[x] : 0;
    }
    return out;
}

namespace {

// ICAO standard atmosphere, troposphere and lower stratosphere. Used to invert a
// cloud-top brightness temperature into a pressure and a geometric height. The
// profile is only a climatology: it assumes a mid-latitude standard lapse rate
// rather than the actual temperature sounding at the observation time, so the
// result is a retrieval candidate, not a radiosonde-quality product.
struct StandardAtmosphere { double pressureHpa; double heightM; };

constexpr double kSeaLevelPressureHpa = 1013.25;
constexpr double kSeaLevelTemperatureK = 288.15;
constexpr double kLapseRateKPerM = 0.0065;
constexpr double kTropopauseHeightM = 11000.0;
constexpr double kTropopauseTemperatureK = 216.65;
constexpr double kStratosphereLapseKPerM = -0.0018;
constexpr double kMaxRetrievedHeightM = 20000.0;

StandardAtmosphere atmosphereFromTemperature(double temperatureK) {
    StandardAtmosphere result{0.0, 0.0};
    if (!(temperatureK > 150.0) || temperatureK > 320.0) return result;
    // Latitude-independent effective profile: the standard troposphere up to the
    // 11 km tropopause, then a weakly cooling lower stratosphere that continues
    // down to the coldest tropical tropopause temperatures actually observed
    // (about 190 K). A purely isothermal upper layer cannot invert those
    // temperatures and would extrapolate cloud tops to unphysical heights.
    const double tropopausePressure = kSeaLevelPressureHpa *
        std::pow(kTropopauseTemperatureK / kSeaLevelTemperatureK, 5.2559);
    if (temperatureK >= kTropopauseTemperatureK) {
        const double ratio = temperatureK / kSeaLevelTemperatureK;
        result.heightM = (kSeaLevelTemperatureK - temperatureK) / kLapseRateKPerM;
        result.pressureHpa = kSeaLevelPressureHpa * std::pow(ratio, 5.2559);
    } else {
        result.heightM = kTropopauseHeightM +
            (kTropopauseTemperatureK - temperatureK) / -kStratosphereLapseKPerM;
        result.pressureHpa = tropopausePressure *
            std::exp(-(result.heightM - kTropopauseHeightM) / 6341.6);
    }
    if (result.heightM > kMaxRetrievedHeightM) {
        result.heightM = kMaxRetrievedHeightM;
        result.pressureHpa = tropopausePressure *
            std::exp(-(kMaxRetrievedHeightM - kTropopauseHeightM) / 6341.6);
    }
    return result;
}

} // namespace

QImage cloudTopPressureFromTbb(const QImage& brightnessTemperature01K,
                               double cloudThresholdKelvin) {
    if (brightnessTemperature01K.isNull()) return {};
    const QImage tbb = brightnessTemperature01K.convertToFormat(QImage::Format_Grayscale16);
    QImage out(tbb.size(), QImage::Format_Grayscale16);
    out.fill(65535);
    const int threshold = qRound(cloudThresholdKelvin * 100.0);
    for (int y = 0; y < tbb.height(); ++y) {
        const auto* src = reinterpret_cast<const quint16*>(tbb.constScanLine(y));
        auto* dst = reinterpret_cast<quint16*>(out.scanLine(y));
        for (int x = 0; x < tbb.width(); ++x) {
            if (src[x] < 15000 || src[x] > threshold) continue; // clear sky: no cloud top
            const StandardAtmosphere atmosphere = atmosphereFromTemperature(src[x] / 100.0);
            if (atmosphere.pressureHpa <= 0.0) continue;
            dst[x] = static_cast<quint16>(qBound(0, qRound(atmosphere.pressureHpa * 10.0), 65534));
        }
    }
    return out;
}

QImage cloudTopHeightFromTbb(const QImage& brightnessTemperature01K,
                             double cloudThresholdKelvin) {
    if (brightnessTemperature01K.isNull()) return {};
    const QImage tbb = brightnessTemperature01K.convertToFormat(QImage::Format_Grayscale16);
    QImage out(tbb.size(), QImage::Format_Grayscale16);
    out.fill(65535);
    const int threshold = qRound(cloudThresholdKelvin * 100.0);
    for (int y = 0; y < tbb.height(); ++y) {
        const auto* src = reinterpret_cast<const quint16*>(tbb.constScanLine(y));
        auto* dst = reinterpret_cast<quint16*>(out.scanLine(y));
        for (int x = 0; x < tbb.width(); ++x) {
            if (src[x] < 15000 || src[x] > threshold) continue;
            const StandardAtmosphere atmosphere = atmosphereFromTemperature(src[x] / 100.0);
            if (atmosphere.heightM <= 0.0) continue;
            dst[x] = static_cast<quint16>(qBound(0, qRound(atmosphere.heightM), 65534));
        }
    }
    return out;
}

namespace {

// Reflectance as a fraction (0..1) from a CALIBRATED reflectance raster, or NaN when the raster
// carries no usable value at this pixel.
//
// Only Format_Grayscale16 is interpreted: that is the calibrated product (hundredths of one
// reflectance percent, 65535 = no data, see visibleReflectanceFactor). Any other format is a
// display image whose scale is unknown, so it is deliberately NOT read as reflectance. An earlier
// version converted whatever it was given to Grayscale8 and divided by 255; for the 16-bit product
// that is a truncation (100 % reflectance became 39/255 = 0.15), which made every reflectance
// threshold below it meaningless.
double reflectanceFractionAt(const QImage& vis, int x, int y) {
    if (vis.isNull() || vis.format() != QImage::Format_Grayscale16) return std::nan("");
    if (x < 0 || y < 0 || x >= vis.width() || y >= vis.height()) return std::nan("");
    const quint16 v = reinterpret_cast<const quint16*>(vis.constScanLine(y))[x];
    return v == 65535 ? std::nan("") : v / 10000.0;
}

// Thresholds of the candidate products below. None of these is fitted to reference data; they are
// the values the earlier code used (or, where noted, the conventional ones) now expressed in real
// reflectance. Keep them together so a fit against official products has one place to change.
constexpr double kSunlitReflectance = 0.01;    // below this the pixel is night or unlit (DN step is 0.47 %)
constexpr double kFogMinDaytimeReflectance = 0.20;  // sunlit low stratus / fog is bright in the visible
constexpr double kLstDarkWaterReflectance = 0.02;   // unchanged from the earlier version
constexpr double kLstCloudReflectance = 0.45;       // sunlit pixel brighter than this is cloud or snow

}  // namespace

QImage fogLowCloudFromTbb(const QImage& ir1Tbb01K, const QImage& ir2Tbb01K,
                          const QImage& visChannel) {
    if (ir1Tbb01K.isNull() || ir2Tbb01K.isNull() || ir1Tbb01K.size() != ir2Tbb01K.size()) return {};
    QImage ir1 = ir1Tbb01K.convertToFormat(QImage::Format_Grayscale16);
    QImage ir2 = ir2Tbb01K.convertToFormat(QImage::Format_Grayscale16);
    // The visible channel is used only when it is a calibrated reflectance raster of the same size
    // (Format_Grayscale16, hundredths of a percent). Anything else is ignored rather than guessed at.
    const bool haveVis = !visChannel.isNull() && visChannel.size() == ir1.size() &&
                         visChannel.format() == QImage::Format_Grayscale16;
    QImage out(ir1.size(), QImage::Format_Grayscale16);
    out.fill(65535);
    for (int y = 0; y < ir1.height(); ++y) {
        const auto* row1 = reinterpret_cast<const quint16*>(ir1.constScanLine(y));
        const auto* row2 = reinterpret_cast<const quint16*>(ir2.constScanLine(y));
        auto* dst = reinterpret_cast<quint16*>(out.scanLine(y));
        for (int x = 0; x < ir1.width(); ++x) {
            if (!validTbb(row1[x]) || !validTbb(row2[x])) continue;
            const double t1 = row1[x] / 100.0;
            const double t2 = row2[x] / 100.0;
            // Low stratus and fog have tops close to the surface temperature and
            // a small 11-12 um brightness-temperature difference. Optically thin
            // high cloud has a much larger split-window difference and is
            // rejected by the warm-top requirement.
            if (t1 < 265.0 || t1 > 295.0) continue;
            const double split = t1 - t2;
            if (split < -1.0 || split > 4.0) continue;
            // Daylight support. Fog and low stratus are BRIGHT in the visible (albedo of order
            // 0.3-0.7) while the clear surfaces that share their temperature -- sea, dark land --
            // are not, so in daylight a bright pixel supports the candidate and a dark one rejects
            // it. The earlier version required the opposite (dark), which kept clear sea and threw
            // away the cloud. Where the pixel is unlit (night) or there is no reflectance raster,
            // the IR test stands alone and the confidence is halved to say so.
            double support = 0.5;
            const double refl = haveVis ? reflectanceFractionAt(visChannel, x, y) : std::nan("");
            if (!std::isnan(refl) && refl >= kSunlitReflectance) {
                if (refl < kFogMinDaytimeReflectance) continue;
                support = 1.0;
            }
            int confidence = qRound(support * 100.0 * (1.0 - std::fabs(split - 1.0) / 5.0));
            confidence = qBound(0, confidence, 100);
            dst[x] = static_cast<quint16>(confidence);
        }
    }
    return out;
}

QImage landSurfaceTemperatureProxy(const QImage& ir1Tbb01K, const QImage& ir2Tbb01K,
                                   const QImage& visChannel) {
    if (ir1Tbb01K.isNull() || ir2Tbb01K.isNull() || ir1Tbb01K.size() != ir2Tbb01K.size()) return {};
    QImage ir1 = ir1Tbb01K.convertToFormat(QImage::Format_Grayscale16);
    QImage ir2 = ir2Tbb01K.convertToFormat(QImage::Format_Grayscale16);
    const bool haveVis = !visChannel.isNull() && visChannel.size() == ir1.size() &&
                         visChannel.format() == QImage::Format_Grayscale16;
    QImage out(ir1.size(), QImage::Format_Grayscale16);
    out.fill(65535);
    for (int y = 0; y < ir1.height(); ++y) {
        const auto* row1 = reinterpret_cast<const quint16*>(ir1.constScanLine(y));
        const auto* row2 = reinterpret_cast<const quint16*>(ir2.constScanLine(y));
        auto* dst = reinterpret_cast<quint16*>(out.scanLine(y));
        for (int x = 0; x < ir1.width(); ++x) {
            if (!validTbb(row1[x]) || !validTbb(row2[x])) continue;
            const double t1 = row1[x] / 100.0;
            const double t2 = row2[x] / 100.0;
            // Clear-sky land is warm and the split-window difference is small.
            if (t1 < 265.0 || t1 > 340.0) continue;
            const double split = t1 - t2;
            if (split < -3.0 || split > 2.0) continue;
            // Visible screening, daylight only (an unlit pixel carries no information and is kept).
            // Very dark pixels are taken to be open water or shadow; bright ones are cloud or snow.
            // The visible alone cannot separate sea from dark vegetation, and there is no land
            // mask here, so this stays a candidate field and not a retrieval.
            const double refl = haveVis ? reflectanceFractionAt(visChannel, x, y) : std::nan("");
            if (!std::isnan(refl) && refl >= kSunlitReflectance) {
                if (refl < kLstDarkWaterReflectance) continue;
                if (refl > kLstCloudReflectance) continue;
            }
            const double lst = t1 + 1.1 * split + 0.6;
            dst[x] = static_cast<quint16>(qBound(0, qRound(lst * 100.0), 65534));
        }
    }
    return out;
}

QImage cloudClassificationFromTbb(const QImage& input) {
    if (input.isNull()) return {};
    const QImage tbb = input.convertToFormat(QImage::Format_Grayscale16);
    QImage out(tbb.size(), QImage::Format_Indexed8);
    QVector<QRgb> colors(256, qRgba(0, 0, 0, 0));
    colors[0] = qRgb(30, 38, 46);
    colors[1] = qRgb(230, 230, 230);
    colors[2] = qRgb(200, 95, 220);
    colors[3] = qRgb(245, 180, 55);
    colors[4] = qRgb(70, 205, 235);
    colors[255] = qRgba(0, 0, 0, 0);
    out.setColorTable(colors);
    for (int y = 0; y < tbb.height(); ++y) {
        const auto* src = reinterpret_cast<const quint16*>(tbb.constScanLine(y));
        auto* dst = out.scanLine(y);
        for (int x = 0; x < tbb.width(); ++x) {
            const int kelvin100 = src[x];
            if (!validTbb(static_cast<quint16>(kelvin100))) dst[x] = 255;
            else if (kelvin100 > 27315) dst[x] = 0;
            else if (kelvin100 <= 23500) dst[x] = 2;
            else dst[x] = 3;
        }
    }
    return out;
}

QImage cloudClassificationFromTbb(const QImage& ir1, const QImage& ir2,
                                  const QImage& ir3, const QImage& ir4) {
    return cloudClassificationFromTbb(ir1, ir2, ir3, ir4, QImage());
}

QImage cloudClassificationFromTbb(const QImage& ir1, const QImage& ir2,
                                  const QImage& ir3, const QImage& ir4,
                                  const QImage& visibleReflectance) {
    // The visible raster is accepted for interface compatibility only. The header once described a
    // sea/land split of the clear class driven by it (Youden J = 0.42 against the official product);
    // that split is not in this function and the parameter has no effect on the result. Without a
    // land/sea mask a visible threshold cannot separate dark vegetation from water, so the clear
    // class stays a single code until a mask is available.
    Q_UNUSED(visibleReflectance);
    QImage classification;
    indexedCloudMask(ir1, ir2, ir3, ir4, &classification);
    return classification;
}

QImage cloudFractionFromTbb(const QImage& input, int cellSize, double cloudThresholdKelvin) {
    if (input.isNull() || cellSize < 1) return {};
    const QImage tbb = input.convertToFormat(QImage::Format_Grayscale16);
    QImage out(tbb.size(), QImage::Format_Grayscale8);
    out.fill(0);
    const int threshold = qRound(cloudThresholdKelvin * 100.0);
    for (int top = 0; top < tbb.height(); top += cellSize) {
        const int bottom = std::min(tbb.height(), top + cellSize);
        for (int left = 0; left < tbb.width(); left += cellSize) {
            const int right = std::min(tbb.width(), left + cellSize);
            quint64 valid = 0, cloudy = 0;
            for (int y = top; y < bottom; ++y) {
                const auto* row = reinterpret_cast<const quint16*>(tbb.constScanLine(y));
                for (int x = left; x < right; ++x) if (row[x] >= 15000 && row[x] <= 35000) {
                    ++valid;
                    if (row[x] <= threshold) ++cloudy;
                }
            }
            if (valid == 0) continue;
            const uchar fraction = static_cast<uchar>(qRound(100.0 * static_cast<double>(cloudy) / valid));
            for (int y = top; y < bottom; ++y) {
                auto* dst = out.scanLine(y);
                std::fill(dst + left, dst + right, fraction);
            }
        }
    }
    return out;
}

QImage cloudFractionFromMask(const QImage& cloudMaskInput, int cellSize) {
    if (cloudMaskInput.isNull() || cellSize < 1) return {};
    const QImage mask = cloudMaskInput.format() == QImage::Format_Indexed8
        ? cloudMaskInput : cloudMaskInput.convertToFormat(QImage::Format_Grayscale8);
    QImage out(mask.size(), QImage::Format_Grayscale8);
    out.fill(255);
    for (int top = 0; top < mask.height(); top += cellSize) {
        const int bottom = std::min(mask.height(), top + cellSize);
        for (int left = 0; left < mask.width(); left += cellSize) {
            const int right = std::min(mask.width(), left + cellSize);
            int valid = 0, cloudy = 0;
            for (int y = top; y < bottom; ++y) {
                const uchar* row = mask.constScanLine(y);
                for (int x = left; x < right; ++x) {
                    if (row[x] > 1) continue;
                    ++valid;
                    if (row[x] == 1) ++cloudy;
                }
            }
            const int cellArea = (bottom - top) * (right - left);
            // Published FY-2-series CTA1 defines Ac=N/4 for a 2x2 unit.
            // A partly received unit has no defensible N/4 because its missing
            // pixels are not known to be clear; leave that whole unit no-data.
            // This also leaves a partial edge block no-data for cellSize > 1.
            if (cellArea != cellSize * cellSize || valid != cellArea) continue;
            const uchar fraction = static_cast<uchar>(qRound(100.0 * cloudy / cellArea));
            for (int y = top; y < bottom; ++y) {
                uchar* row = out.scanLine(y);
                std::fill(row + left, row + right, fraction);
            }
        }
    }
    return out;
}

QImage cloudFractionSlidingFromMask(const QImage& cloudMaskInput, int windowSize) {
    if (cloudMaskInput.isNull() || windowSize < 1 || (windowSize % 2) == 0) return {};
    const QImage mask = cloudMaskInput.format() == QImage::Format_Indexed8
        ? cloudMaskInput : cloudMaskInput.convertToFormat(QImage::Format_Grayscale8);
    QImage out(mask.size(), QImage::Format_Grayscale8);
    out.fill(255);
    const int radius = windowSize / 2;
    const int area = windowSize * windowSize;
    for (int y = radius; y < mask.height() - radius; ++y) {
        uchar* dst = out.scanLine(y);
        for (int x = radius; x < mask.width() - radius; ++x) {
            int cloudy = 0, valid = 0;
            for (int wy = y - radius; wy <= y + radius; ++wy) {
                const uchar* row = mask.constScanLine(wy);
                for (int wx = x - radius; wx <= x + radius; ++wx) {
                    if (row[wx] > 1) continue;
                    ++valid;
                    if (row[wx] == 1) ++cloudy;
                }
            }
            if (valid == area) dst[x] = static_cast<uchar>(qRound(100.0 * cloudy / area));
        }
    }
    return out;
}

QImage applyProcessing(const QImage& input, bool lineRepair, bool dotRepair, bool destripe) {
    QImage out = input;
    if (lineRepair) out = repairMissingLines(out);
    if (dotRepair) out = medianBlur(out, 3);
    if (destripe) out = destripeConservative(out);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Additional diagnostic products.
//
// Everything below is derived from calibrated brightness-temperature rasters (Format_Grayscale16,
// hundredths of a kelvin, validTbb() gates the physical range). They are DIAGNOSTIC / CANDIDATE
// products: each follows a conventional multispectral test, none has its thresholds fitted to a
// reference set, and each says so in its documentation in imageproducts.h. They are not retrievals.
// ---------------------------------------------------------------------------------------------
namespace {

constexpr quint16 kNoData16 = 65535;

QImage asGray16(const QImage& image) {
    return image.format() == QImage::Format_Grayscale16
        ? image : image.convertToFormat(QImage::Format_Grayscale16);
}

bool sameSizeNonNull(const QImage& a, const QImage& b) {
    return !a.isNull() && !b.isNull() && a.size() == b.size();
}

// Piecewise-linear colour from a value and a set of stops.
QRgb gradientColour(double value, const double* stops, const QColor* colours, int count) {
    if (value <= stops[0]) return colours[0].rgb();
    if (value >= stops[count - 1]) return colours[count - 1].rgb();
    int hi = 1;
    while (hi < count - 1 && value > stops[hi]) ++hi;
    const int lo = hi - 1;
    const double mix = (value - stops[lo]) / (stops[hi] - stops[lo]);
    return qRgb(qRound(colours[lo].red() * (1.0 - mix) + colours[hi].red() * mix),
                qRound(colours[lo].green() * (1.0 - mix) + colours[hi].green() * mix),
                qRound(colours[lo].blue() * (1.0 - mix) + colours[hi].blue() * mix));
}

// Convective tests, kelvin. Conventional values for mesoscale convective systems and overshooting
// tops; NOT fitted to this instrument.
constexpr double kColdCloudK = 233.15;            // -40 C
constexpr double kDeepConvectionK = 218.15;       // -55 C
constexpr double kOvershootK = 208.15;            // -65 C
constexpr double kDeepConvectionWvIrwK = -5.0;    // thick cold cloud: WV-IRW close to zero; thin cirrus: far negative
constexpr double kOvershootWvIrwK = 0.0;          // WV-IRW >= 0 flags tops above the WV weighting level

// ISCCP cloud-top pressure boundaries, hPa.
constexpr double kLowMidBoundaryHpa = 680.0;
constexpr double kMidHighBoundaryHpa = 440.0;

// Night fog: fog droplets emit less at 3.7 um than at 10.8 um, so IR1 - IR4 is positive over fog and
// near zero over clear sky. In daylight reflected sunlight raises IR4 and the test under-detects.
constexpr double kNightFogMinDiffK = 1.5;
constexpr double kNightFogFullDiffK = 5.5;
constexpr double kFogMinTopK = 265.0;
constexpr double kFogMaxTopK = 295.0;

}  // namespace

QImage brightnessTemperatureDifference(const QImage& minuend, const QImage& subtrahend) {
    if (!sameSizeNonNull(minuend, subtrahend)) return {};
    const QImage a = asGray16(minuend), b = asGray16(subtrahend);
    QImage out(a.size(), QImage::Format_Grayscale16);
    out.fill(kNoData16);
    for (int y = 0; y < a.height(); ++y) {
        const auto* ra = reinterpret_cast<const quint16*>(a.constScanLine(y));
        const auto* rb = reinterpret_cast<const quint16*>(b.constScanLine(y));
        auto* dst = reinterpret_cast<quint16*>(out.scanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            if (!validTbb(ra[x]) || !validTbb(rb[x])) continue;
            const double diffK = (static_cast<int>(ra[x]) - static_cast<int>(rb[x])) / 100.0;
            const double stored = (diffK + kBtdOffsetKelvin) * 100.0;
            dst[x] = static_cast<quint16>(qBound(0, qRound(stored), 65534));
        }
    }
    return out;
}

QImage btdColourEnhancement(const QImage& btdRaster, double minKelvin, double maxKelvin) {
    if (btdRaster.isNull() || !(minKelvin < 0.0) || !(maxKelvin > 0.0)) return {};
    const QImage btd = asGray16(btdRaster);
    QImage out(btd.size(), QImage::Format_RGB32);
    for (int y = 0; y < btd.height(); ++y) {
        const auto* src = reinterpret_cast<const quint16*>(btd.constScanLine(y));
        auto* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < btd.width(); ++x) {
            if (src[x] == kNoData16) { dst[x] = qRgb(0, 0, 0); continue; }
            const double k = src[x] / 100.0 - kBtdOffsetKelvin;
            // Diverging: blue for negative, white at zero, red for positive.
            if (k < 0.0) {
                const double t = std::clamp(k / minKelvin, 0.0, 1.0);
                dst[x] = qRgb(qRound(250 - 215 * t), qRound(250 - 150 * t), qRound(250 - 35 * t));
            } else {
                const double t = std::clamp(k / maxKelvin, 0.0, 1.0);
                dst[x] = qRgb(qRound(250 - 40 * t), qRound(250 - 205 * t), qRound(250 - 215 * t));
            }
        }
    }
    return out;
}

QImage convectiveCloudCandidates(const QImage& ir1Tbb01K, const QImage& ir3Tbb01K) {
    if (!sameSizeNonNull(ir1Tbb01K, ir3Tbb01K)) return {};
    const QImage ir1 = asGray16(ir1Tbb01K), ir3 = asGray16(ir3Tbb01K);
    QImage out(ir1.size(), QImage::Format_Indexed8);
    QVector<QRgb> colours(256, qRgba(0, 0, 0, 0));
    colours[0] = qRgb(30, 38, 46);        // not cold or not convective
    colours[1] = qRgb(120, 170, 230);     // cold cloud top (<= 233.15 K)
    colours[2] = qRgb(245, 180, 55);      // deep convection candidate
    colours[3] = qRgb(235, 60, 70);       // overshooting top candidate
    colours[255] = qRgb(115, 125, 138);   // invalid / no data
    out.setColorTable(colours);
    for (int y = 0; y < ir1.height(); ++y) {
        const auto* r1 = reinterpret_cast<const quint16*>(ir1.constScanLine(y));
        const auto* r3 = reinterpret_cast<const quint16*>(ir3.constScanLine(y));
        auto* dst = out.scanLine(y);
        for (int x = 0; x < ir1.width(); ++x) {
            if (!validTbb(r1[x]) || !validTbb(r3[x])) { dst[x] = 255; continue; }
            const double t1 = r1[x] / 100.0;
            const double wvIrw = r3[x] / 100.0 - t1;
            if (t1 <= kOvershootK && wvIrw >= kOvershootWvIrwK) dst[x] = 3;
            else if (t1 <= kDeepConvectionK && wvIrw >= kDeepConvectionWvIrwK) dst[x] = 2;
            else if (t1 <= kColdCloudK) dst[x] = 1;
            else dst[x] = 0;
        }
    }
    return out;
}

QImage cloudLevelFromTbb(const QImage& tbb01K, const QImage& cloudMask) {
    if (tbb01K.isNull()) return {};
    const QImage tbb = asGray16(tbb01K);
    const bool haveMask = !cloudMask.isNull() && cloudMask.size() == tbb.size() &&
                          cloudMask.format() == QImage::Format_Indexed8;
    QImage out(tbb.size(), QImage::Format_Indexed8);
    QVector<QRgb> colours(256, qRgba(0, 0, 0, 0));
    colours[0] = qRgb(30, 38, 46);        // clear
    colours[1] = qRgb(245, 180, 55);      // low   (> 680 hPa)
    colours[2] = qRgb(120, 200, 120);     // mid   (440-680 hPa)
    colours[3] = qRgb(200, 95, 220);      // high  (< 440 hPa)
    colours[255] = qRgb(115, 125, 138);   // invalid / no data
    out.setColorTable(colours);
    for (int y = 0; y < tbb.height(); ++y) {
        const auto* src = reinterpret_cast<const quint16*>(tbb.constScanLine(y));
        const uchar* maskRow = haveMask ? cloudMask.constScanLine(y) : nullptr;
        auto* dst = out.scanLine(y);
        for (int x = 0; x < tbb.width(); ++x) {
            if (!validTbb(src[x])) { dst[x] = 255; continue; }
            bool cloudy;
            if (maskRow) {
                if (maskRow[x] > 1) { dst[x] = 255; continue; }   // mask says invalid
                cloudy = maskRow[x] == 1;
            } else {
                cloudy = src[x] <= 27315;   // same single-channel proxy the other products use
            }
            if (!cloudy) { dst[x] = 0; continue; }
            const StandardAtmosphere atmosphere = atmosphereFromTemperature(src[x] / 100.0);
            if (atmosphere.pressureHpa <= 0.0) { dst[x] = 255; continue; }
            dst[x] = atmosphere.pressureHpa > kLowMidBoundaryHpa ? 1
                   : atmosphere.pressureHpa > kMidHighBoundaryHpa ? 2 : 3;
        }
    }
    return out;
}

QImage nightFogCandidatesFromTbb(const QImage& ir1Tbb01K, const QImage& ir4Tbb01K,
                                 const QImage& visibleReflectance) {
    if (!sameSizeNonNull(ir1Tbb01K, ir4Tbb01K)) return {};
    const QImage ir1 = asGray16(ir1Tbb01K), ir4 = asGray16(ir4Tbb01K);
    const bool haveVis = !visibleReflectance.isNull() && visibleReflectance.size() == ir1.size() &&
                         visibleReflectance.format() == QImage::Format_Grayscale16;
    QImage out(ir1.size(), QImage::Format_Grayscale16);
    out.fill(kNoData16);
    for (int y = 0; y < ir1.height(); ++y) {
        const auto* r1 = reinterpret_cast<const quint16*>(ir1.constScanLine(y));
        const auto* r4 = reinterpret_cast<const quint16*>(ir4.constScanLine(y));
        auto* dst = reinterpret_cast<quint16*>(out.scanLine(y));
        for (int x = 0; x < ir1.width(); ++x) {
            if (!validTbb(r1[x]) || !validTbb(r4[x])) continue;
            const double t1 = r1[x] / 100.0;
            if (t1 < kFogMinTopK || t1 > kFogMaxTopK) continue;
            // IR4 is contaminated by reflected sunlight, so a sunlit pixel is not tested at all.
            if (haveVis) {
                const double refl = reflectanceFractionAt(visibleReflectance, x, y);
                if (!std::isnan(refl) && refl >= kSunlitReflectance) continue;
            }
            const double diff = t1 - r4[x] / 100.0;
            if (diff < kNightFogMinDiffK) continue;
            const double frac = std::clamp((diff - kNightFogMinDiffK) /
                                           (kNightFogFullDiffK - kNightFogMinDiffK), 0.0, 1.0);
            dst[x] = static_cast<quint16>(qRound(20.0 + 80.0 * frac));
        }
    }
    return out;
}

QImage waterVapourColourEnhancement(const QImage& ir3Tbb01K) {
    if (ir3Tbb01K.isNull()) return {};
    const QImage tbb = asGray16(ir3Tbb01K);
    QImage out(tbb.size(), QImage::Format_RGB32);
    // Cold / moist upper troposphere is white to blue, warm / dry is orange to brown.
    constexpr double stops[7] = {200.0, 220.0, 235.0, 248.0, 258.0, 270.0, 285.0};
    const QColor colours[7] = {QColor(255, 255, 255), QColor(130, 205, 255), QColor(45, 125, 225),
                               QColor(50, 165, 120), QColor(215, 205, 90), QColor(235, 145, 55),
                               QColor(150, 65, 40)};
    for (int y = 0; y < tbb.height(); ++y) {
        const auto* src = reinterpret_cast<const quint16*>(tbb.constScanLine(y));
        auto* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < tbb.width(); ++x) {
            if (!validTbb(src[x])) { dst[x] = qRgb(0, 0, 0); continue; }
            dst[x] = gradientColour(src[x] / 100.0, stops, colours, 7);
        }
    }
    return out;
}
}








