#include "globecore.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace nadirstation::globe {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
// Geostationary radius over the equatorial radius, as in gk2aproducts.cpp (spherical Earth).
constexpr double kSatRadius = 42164.0 / 6378.137;

double smoothstep(double a, double b, double x) {
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

struct Colour { double r, g, b; };

Colour sampleRgb(const Rgb8& im, double x, double y, bool wrapX) {
    // Bilinear, pixel centres at integer + 0.5.
    x -= 0.5; y -= 0.5;
    int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const double fx = x - x0, fy = y - y0;
    auto at = [&](int xi, int yi) {
        if (wrapX) { xi %= im.width; if (xi < 0) xi += im.width; }
        else xi = std::clamp(xi, 0, im.width - 1);
        yi = std::clamp(yi, 0, im.height - 1);
        const std::uint8_t* p = im.data.data() + (static_cast<std::size_t>(yi) * im.width + xi) * 3;
        return Colour{double(p[0]), double(p[1]), double(p[2])};
    };
    const Colour a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
    auto mix = [&](double p, double q, double r, double s) {
        return (p * (1 - fx) + q * fx) * (1 - fy) + (r * (1 - fx) + s * fx) * fy;
    };
    return {mix(a.r, b.r, c.r, d.r), mix(a.g, b.g, c.g, d.g), mix(a.b, b.b, c.b, d.b)};
}

Colour relief(const Scene& s, double lat, double lon) {
    if (!s.relief || s.relief->empty()) {
        // No map: a plain ocean so the clouds still read.
        return {14, 32, 58};
    }
    const double u = (lon + 180.0) / 360.0 * s.relief->width;
    const double v = (90.0 - lat) / 180.0 * s.relief->height;
    return sampleRgb(*s.relief, u, v, true);
}

std::uint32_t hash(std::uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}
}  // namespace

bool enhancedTopColour(double k, double* r, double* g, double* b) {
    // Cold cloud tops by temperature, warm to cold: light blue, blue, green, yellow, red, magenta,
    // white for the coldest overshooting tops. Linear between the stops.
    struct Stop { double k, r, g, b; };
    static const Stop stops[] = {
        {240.0, 150, 205, 255}, {230.0, 40, 110, 245}, {220.0, 40, 200, 70}, {210.0, 250, 230, 40},
        {200.0, 245, 90, 25}, {190.0, 215, 30, 160}, {180.0, 255, 255, 255},
    };
    if (!(k < stops[0].k)) return false;
    for (std::size_t i = 1; i < std::size(stops); ++i) {
        if (k >= stops[i].k) {
            const double t = (stops[i - 1].k - k) / (stops[i - 1].k - stops[i].k);
            *r = stops[i - 1].r + (stops[i].r - stops[i - 1].r) * t;
            *g = stops[i - 1].g + (stops[i].g - stops[i - 1].g) * t;
            *b = stops[i - 1].b + (stops[i].b - stops[i - 1].b) * t;
            return true;
        }
    }
    const Stop& last = stops[std::size(stops) - 1];
    *r = last.r; *g = last.g; *b = last.b;
    return true;
}

double globeRadius(const View& v) {
    return 0.46 * std::min(v.width, v.height) * std::max(0.05, v.zoom);
}

bool screenToLatLon(const View& v, double sx, double sy, double* latDeg, double* lonDeg) {
    const double r = globeRadius(v);
    const double x = (sx - v.width * 0.5) / r;
    const double y = (v.height * 0.5 - sy) / r;
    const double r2 = x * x + y * y;
    if (r2 > 1.0) return false;
    const double z = std::sqrt(1.0 - r2);
    const double p0 = v.centreLatDeg * kDeg;
    const double sp = std::sin(p0), cp = std::cos(p0);
    *latDeg = std::asin(std::clamp(y * cp + z * sp, -1.0, 1.0)) / kDeg;
    double lon = v.centreLonDeg + std::atan2(x, z * cp - y * sp) / kDeg;
    lon = std::fmod(lon + 540.0, 360.0) - 180.0;
    *lonDeg = lon;
    return true;
}

bool latLonToScreen(const View& v, double latDeg, double lonDeg, double* sx, double* sy) {
    const double p = latDeg * kDeg, dl = (lonDeg - v.centreLonDeg) * kDeg;
    const double p0 = v.centreLatDeg * kDeg;
    const double cosc = std::sin(p0) * std::sin(p) + std::cos(p0) * std::cos(p) * std::cos(dl);
    if (cosc < 0.0) return false;
    const double x = std::cos(p) * std::sin(dl);
    const double y = std::cos(p0) * std::sin(p) - std::sin(p0) * std::cos(p) * std::cos(dl);
    const double r = globeRadius(v);
    *sx = v.width * 0.5 + x * r;
    *sy = v.height * 0.5 - y * r;
    return true;
}

bool diskPixel(const DiskGeometry& g, double latDeg, double lonDeg, double* col, double* row, double* edge) {
    if (!g.valid()) return false;
    const double lat = latDeg * kDeg, dl = (lonDeg - g.subLonDeg) * kDeg;
    const double x = std::cos(lat) * std::cos(dl);
    const double y = std::cos(lat) * std::sin(dl);
    const double z = std::sin(lat);
    const double vx = kSatRadius - x;
    const double range = std::sqrt(vx * vx + y * y + z * z);
    // Cosine of the satellite zenith angle: the line of sight against the local vertical.
    const double cosZen = (kSatRadius * x - 1.0) / range;
    if (cosZen <= 0.0) return false;
    *col = g.centre + std::atan2(y, vx) / g.radPerPixel;
    *row = g.centre - std::asin(z / range) / g.radPerPixel;
    *edge = cosZen;
    return true;
}

double tbbAt(const Scene& s, double latDeg, double lonDeg) {
    if (!s.tbb || s.tbb->empty()) return std::nan("");
    double c, r, e;
    if (!diskPixel(s.disk, latDeg, lonDeg, &c, &r, &e)) return std::nan("");
    const int xi = static_cast<int>(c), yi = static_cast<int>(r);
    if (xi < 0 || yi < 0 || xi >= s.tbb->width || yi >= s.tbb->height) return std::nan("");
    const std::uint16_t raw = s.tbb->data[static_cast<std::size_t>(yi) * s.tbb->width + xi];
    return raw == 0 ? std::nan("") : raw * 0.01;
}

double sampleTbb(const Grey16& g, const DiskGeometry& d, double latDeg, double lonDeg, int radius) {
    if (g.empty()) return std::nan("");
    double col, row, edge;
    if (!diskPixel(d, latDeg, lonDeg, &col, &row, &edge)) return std::nan("");
    const int cx = static_cast<int>(col), cy = static_cast<int>(row);
    double sum = 0.0;
    int n = 0;
    for (int y = cy - radius; y <= cy + radius; ++y)
        for (int x = cx - radius; x <= cx + radius; ++x) {
            if (x < 0 || y < 0 || x >= g.width || y >= g.height) continue;
            const std::uint16_t v = g.data[static_cast<std::size_t>(y) * g.width + x];
            if (v != 0) { sum += v * 0.01; ++n; }
        }
    return n ? sum / n : std::nan("");
}

void renderRows(const Scene& s, const View& v, int y0, int y1, int step, std::uint8_t* rgba) {
    step = std::max(1, step);
    const double r = globeRadius(v);
    const double cx = v.width * 0.5, cy = v.height * 0.5;
    const double p0 = v.centreLatDeg * kDeg;
    const double sp0 = std::sin(p0), cp0 = std::cos(p0);
    double sunX = 0, sunY = 0, sunZ = 0;
    if (s.sunKnown) {
        const double a = s.sunLatDeg * kDeg, b = s.sunLonDeg * kDeg;
        sunX = std::cos(a) * std::cos(b); sunY = std::cos(a) * std::sin(b); sunZ = std::sin(a);
    }
    const bool haveTbb = s.tbb && !s.tbb->empty() && s.disk.valid();
    const bool geoProduct = s.product && !s.product->empty() && s.geo.valid();
    const bool haveProduct = s.product && !s.product->empty() && s.disk.valid() && !geoProduct;

    for (int y = y0 - (y0 % step); y < y1; y += step) {
        for (int x = 0; x < v.width; x += step) {
            const double px = (x + 0.5 * step - cx) / r;
            const double py = (cy - (y + 0.5 * step)) / r;
            const double r2 = px * px + py * py;
            double cr, cg, cb;
            if (r2 > 1.0) {
                // Space: a dark field, a few fixed stars, and the atmosphere's glow at the limb.
                const double d = std::sqrt(r2);
                cr = 5; cg = 8; cb = 13;
                const std::uint32_t h = hash(static_cast<std::uint32_t>(x / step) * 73856093u ^
                                             static_cast<std::uint32_t>(y / step) * 19349663u);
                if ((h & 1023u) < 2u) { const double s0 = 90 + (h >> 24) % 140; cr = cg = cb = s0; }
                if (d < 1.09) {
                    const double glow = std::pow(1.0 - (d - 1.0) / 0.09, 2.2) * 0.75;
                    cr += 70 * glow; cg += 135 * glow; cb += 230 * glow;
                }
            } else {
                const double z = std::sqrt(1.0 - r2);
                const double lat = std::asin(std::clamp(py * cp0 + z * sp0, -1.0, 1.0)) / kDeg;
                double lon = v.centreLonDeg + std::atan2(px, z * cp0 - py * sp0) / kDeg;
                lon = std::fmod(lon + 540.0, 360.0) - 180.0;

                Colour base = relief(s, lat, lon);
                double day = 1.0;
                if (s.night && s.sunKnown) {
                    const double a = lat * kDeg, b = lon * kDeg;
                    const double cosz = std::cos(a) * std::cos(b) * sunX + std::cos(a) * std::sin(b) * sunY + std::sin(a) * sunZ;
                    day = smoothstep(-0.12, 0.10, cosz);
                }
                double col = 0, row = 0, edge = 0;
                const bool seen = (haveTbb || haveProduct) && diskPixel(s.disk, lat, lon, &col, &row, &edge);
                // Towards the limb the satellite looks through more atmosphere and the surface reads colder,
                // which would show as a white haze; the disk is faded out over that band instead.
                const double fade = seen ? smoothstep(0.12, 0.38, edge) : 0.0;

                if (s.layer == Layer::TrueColour || s.layer == Layer::EnhancedTops) {
                    const double dim = 0.16 + 0.84 * day;
                    cr = base.r * dim; cg = base.g * dim; cb = base.b * dim;
                    if (seen && haveTbb) {
                        const int xi = static_cast<int>(col), yi = static_cast<int>(row);
                        if (xi >= 0 && yi >= 0 && xi < s.tbb->width && yi < s.tbb->height) {
                            const std::uint16_t raw = s.tbb->data[static_cast<std::size_t>(yi) * s.tbb->width + xi];
                            if (raw != 0) {
                                const double k = raw * 0.01;
                                const double cov = std::clamp((300.0 - k) / (300.0 - 205.0), 0.0, 1.0);
                                const double w = std::pow(cov, 1.4) * 0.97 * fade;
                                const double lum = (0.66 + 0.34 * cov) * 255.0 * (0.5 + 0.5 * day);
                                cr = cr * (1 - w) + lum * 0.97 * w;
                                cg = cg * (1 - w) + lum * 0.99 * w;
                                cb = cb * (1 - w) + lum * 1.00 * w;
                                double er, eg, eb;
                                if (s.layer == Layer::EnhancedTops && enhancedTopColour(k, &er, &eg, &eb)) {
                                    // Fades in over the first 6 K below 240 K so the edge is soft;
                                    // darkened a little on the night side like the clouds.
                                    const double a = std::clamp((240.0 - k) / 6.0, 0.0, 1.0) * 0.9 * fade;
                                    const double lit = 0.55 + 0.45 * day;
                                    cr = cr * (1 - a) + er * lit * a;
                                    cg = cg * (1 - a) + eg * lit * a;
                                    cb = cb * (1 - a) + eb * lit * a;
                                }
                            }
                        }
                    }
                } else {
                    const double dim = 0.35 * (0.4 + 0.6 * day);
                    cr = base.r * dim; cg = base.g * dim; cb = base.b * dim;
                    if (seen && haveProduct) {
                        const Colour p = sampleRgb(*s.product, col + 0.5, row + 0.5, false);
                        // Black is "no data" in the disk products (space, missing strips).
                        if (p.r + p.g + p.b > 6.0) {
                            cr = cr * (1 - fade) + p.r * fade;
                            cg = cg * (1 - fade) + p.g * fade;
                            cb = cb * (1 - fade) + p.b * fade;
                        }
                    } else if (geoProduct) {
                        // Lat/lon image: column from longitude (taken east of the image's west edge),
                        // row from latitude; nothing outside the box.
                        const double west = s.geo.lon0 - 0.5 * s.geo.dLon;
                        double dl = std::fmod(lon - west, 360.0);
                        if (dl < 0) dl += 360.0;
                        const double gx = dl / s.geo.dLon;
                        const double gy = (s.geo.lat0 + 0.5 * s.geo.dLat - lat) / s.geo.dLat;
                        if (gx >= 0 && gy >= 0 && gx < s.product->width && gy < s.product->height) {
                            const Colour p = sampleRgb(*s.product, gx, gy, false);
                            if (p.r + p.g + p.b > 6.0) { cr = p.r; cg = p.g; cb = p.b; }
                        }
                    }
                }
                // Gentle limb darkening and the blue rim of the atmosphere.
                const double shade = 0.80 + 0.20 * z;
                const double rim = std::pow(1.0 - z, 3.0) * 0.55;
                cr = cr * shade + 70 * rim; cg = cg * shade + 130 * rim; cb = cb * shade + 225 * rim;
            }
            const std::uint8_t R = static_cast<std::uint8_t>(std::clamp(cr, 0.0, 255.0));
            const std::uint8_t G = static_cast<std::uint8_t>(std::clamp(cg, 0.0, 255.0));
            const std::uint8_t B = static_cast<std::uint8_t>(std::clamp(cb, 0.0, 255.0));
            for (int yy = y; yy < std::min(y + step, y1); ++yy) {
                if (yy < y0) continue;
                for (int xx = x; xx < std::min(x + step, v.width); ++xx) {
                    std::uint8_t* o = rgba + (static_cast<std::size_t>(yy) * v.width + xx) * 4;
                    o[0] = R; o[1] = G; o[2] = B; o[3] = 255;
                }
            }
        }
    }
}

void subSolarPoint(double unixSeconds, double* latDeg, double* lonDeg) {
    // Low-precision solar position (Astronomical Almanac), good to about 0.01 degree.
    const double n = unixSeconds / 86400.0 - 10957.5;   // days since J2000.0
    const double L = std::fmod(280.460 + 0.9856474 * n, 360.0);
    const double g = std::fmod(357.528 + 0.9856003 * n, 360.0) * kDeg;
    const double lambda = (L + 1.915 * std::sin(g) + 0.020 * std::sin(2 * g)) * kDeg;
    const double eps = (23.439 - 0.0000004 * n) * kDeg;
    const double decl = std::asin(std::sin(eps) * std::sin(lambda));
    const double ra = std::atan2(std::cos(eps) * std::sin(lambda), std::cos(lambda)) / kDeg;
    const double gmst = std::fmod(280.46061837 + 360.98564736629 * n, 360.0);
    double lon = ra - gmst;
    lon = std::fmod(lon + 540.0, 360.0);
    if (lon < 0) lon += 360.0;
    *latDeg = decl / kDeg;
    *lonDeg = lon - 180.0;
}

}  // namespace nadirstation::globe
