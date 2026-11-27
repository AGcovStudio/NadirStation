// MetOp map products of MHS, AMSU-A and the IASI imager: brightness temperature from each scan's
// on-board calibration (space view and warm load, as in the NOAA KLM User's Guide, sections 7.3 and
// 7.6), located on the Earth from the TLE and the packet times, drawn on a latitude / longitude grid.
#include "polarinstruments.h"

#include "geomap.h"

#include <QDateTime>
#include <QDir>
#include <QTimeZone>
#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace nadirstation::polar {
namespace {
#include "mwcal_coefs.inc"

constexpr double kC1 = 1.1910427e-05, kC2 = 1.4387752;   // mW/(m2 sr cm-1), cm-1
double planck(double t, double nu) { return kC1 * nu * nu * nu / (std::exp(kC2 * nu / t) - 1.0); }
double invPlanck(double r, double nu) { return r > 0 ? kC2 * nu / std::log(kC1 * nu * nu * nu / r + 1.0) : std::nan(""); }
// piecewise linear through three (x, y) points, extended beyond them
double interp3(const double* xs, const double* ys, double x) {
    const int k = x < xs[1] ? 0 : 1;
    const double d = xs[k + 1] - xs[k];
    return d == 0 ? ys[k] : ys[k] + (ys[k + 1] - ys[k]) * (x - xs[k]) / d;
}
int satIndex(int scid) { return scid == 11 ? 0 : scid == 13 ? 1 : -1; }

// the 1st / 99th percentile of a grid band, rounded outwards to `step`
std::pair<double, double> range(const GeoGrid& g, int band, double step) {
    std::vector<float> v;
    for (int y = 0; y < g.height(); ++y) for (int x = 0; x < g.width(); ++x) { const float a = g.at(x, y, band); if (std::isfinite(a)) v.push_back(a); }
    if (v.empty()) return {0, 1};
    std::sort(v.begin(), v.end());
    double lo = std::floor(v[v.size() / 100] / step) * step, hi = std::ceil(v[v.size() * 99 / 100] / step) * step;
    if (hi <= lo) hi = lo + step;
    return {lo, hi};
}
}  // namespace

QStringList MetopInstruments::saveMaps(const QString& dir) const {
    QStringList out;
    QStringList notes;
    SwathGeo geo(ascatTleName_, ascatTle1_, ascatTle2_);
    if (!geo.valid()) { mapsLog_ = QStringLiteral("maps: no TLE"); return out; }
    const int si = satIndex(scid_);
    const QString sat = scid_ == 11 ? QStringLiteral("MetOp-B") : scid_ == 13 ? QStringLiteral("MetOp-C") : QStringLiteral("MetOp");
    auto when = [](double t0, double t1) {
        return QDateTime::fromMSecsSinceEpoch(qint64(t0 * 1000), QTimeZone::UTC).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) + QStringLiteral(" - ")
             + QDateTime::fromMSecsSinceEpoch(qint64(t1 * 1000), QTimeZone::UTC).toString(QStringLiteral("HH:mm:ss 'UTC'"));
    };
    auto writeMap = [&](const GeoGrid& g, int band, const QString& name, const QString& title, const QString& detail, int zoom,
                        std::pair<double, double> r, const std::function<QRgb(double)>& ramp, const QString& unit, bool rawK) {
        GeoGrid::Legend L;
        L.title = title;
        L.detail = detail;
        L.ramp = ramp;
        L.lowLabel = QStringLiteral("%1 %2").arg(r.first).arg(unit);
        L.midLabel = QStringLiteral("%1").arg((r.first + r.second) / 2);
        L.highLabel = QStringLiteral("%1 %2").arg(r.second).arg(unit);
        const QImage img = g.render([&](const GeoGrid& gg, int x, int y) -> QRgb {
            const float v = gg.at(x, y, band);
            if (!std::isfinite(v)) return 0;
            return ramp((v - r.first) / (r.second - r.first));
        }, zoom, mapsDir_, L);
        const QString p = QDir(dir).filePath(name + QStringLiteral("_Map.png"));
        if (img.save(p, "PNG")) out << p;
        if (rawK) {
            const QString q = QDir(dir).filePath(name + QStringLiteral("_Map_TBB_0p01K.png"));
            if (g.saveRaw16(band, 100.0, 0.0, q)) out << q;
        }
    };

    // ---------------------------------------------------------------- MHS
    if (si >= 0 && mhs_.size() >= 8) {
        const MhsCoefs& k = kMhsCoefs[si];
        const int n = int(mhs_.size());
        int pieVotes = 0;
        for (const auto& s : mhs_) pieVotes += s.pie;
        const int pie = pieVotes * 2 > n ? 1 : 0;
        const int bk = k.backup ? 1 : 0;
        // views smoothed over seven scans (weights 1 2 3 4 3 2 1), missing views left out
        std::vector<std::array<std::array<double, 2>, 5>> views;
        views.resize(std::size_t(n));
        for (int l = 0; l < n; ++l)
            for (int c = 0; c < 5; ++c)
                for (int j = 0; j < 2; ++j) {
                    double sum = 0, w = 0;
                    for (int d = -3; d <= 3; ++d) {
                        const int m = l + d;
                        if (m < 0 || m >= n) continue;
                        const int v = mhs_[std::size_t(m)].views[std::size_t(c)][std::size_t(j)];
                        if (!v) continue;
                        sum += v * k.avgWeights[d + 3];
                        w += k.avgWeights[d + 3];
                    }
                    views[std::size_t(l)][std::size_t(c)][std::size_t(j)] = w > 0 ? sum / w : 0.0;
                }
        GeoGrid g(5);
        double lastTw = 0, t0 = 1e300, t1 = 0;
        int calibrated = 0;
        for (int l = 0; l < n; ++l) {
            const auto& s = mhs_[std::size_t(l)];
            if (s.time <= 0) continue;
            // blackbody temperature from the PRTs (resistance by a line through the calibration resistors)
            const double* rc = k.RCAL[pie];
            const double cn = s.prtCal[0] + s.prtCal[1] + s.prtCal[2];
            const double c2 = double(s.prtCal[0]) * s.prtCal[0] + double(s.prtCal[1]) * s.prtCal[1] + double(s.prtCal[2]) * s.prtCal[2];
            const double rn = rc[0] + rc[1] + rc[2];
            const double rcn = s.prtCal[0] * rc[0] + s.prtCal[1] * rc[1] + s.prtCal[2] * rc[2];
            const double den = 3 * c2 - cn * cn;
            double tw = 0, ww = 0;
            if (den != 0) {
                const double a = (rn * c2 - cn * rcn) / den, b = (3 * rcn - rn * cn) / den;
                for (int i = 0; i < 5; ++i) {
                    const double R = a + b * s.prt[std::size_t(i)];
                    double T = 0;
                    for (int j = 0; j < 4; ++j) T += k.f[pie][i][j] * std::pow(R, j);
                    if (T > 260 && T < 310) { tw += k.W[i] * T; ww += k.W[i]; }
                }
            }
            const double Tw = ww > 0 ? tw / ww : lastTw;
            if (ww > 0) lastTw = Tw;
            if (Tw <= 0) continue;
            double tinst = 0;
            const double hk = s.hk[std::size_t(9 + (bk ? 3 : 0))];
            for (int j = 0; j < 5; ++j) tinst += k.g[j] * std::pow(hk, j);
            std::array<double, 3> a0{}, a1{}, a2{};
            std::array<std::array<double, 3>, 5> coef{};
            bool ok = true;
            for (int c = 0; c < 5; ++c) {
                const double nu = k.wavenumber[c];
                const double Twp = k.corr[c][0] + k.corr[c][1] * Tw;
                const double Rw = planck(Twp, nu), Rc = planck(2.73 + k.csCorr[k.csCorrId][c], nu);
                const double Cs = views[std::size_t(l)][std::size_t(c)][0], Cw = views[std::size_t(l)][std::size_t(c)][1];
                if (Cs <= 0 || Cw <= 0 || Cw == Cs) { ok = false; break; }
                const double G = (Cw - Cs) / (Rw - Rc);
                double ys[3] = {k.u[bk][0][c], k.u[bk][1][c], k.u[bk][2][c]};
                const double u = interp3(k.uTemps[bk], ys, tinst);
                coef[std::size_t(c)] = {Rw - Cw / G + u * Cw * Cs / (G * G), 1.0 / G - u * (Cs + Cw) / (G * G), u / (G * G)};
            }
            (void)a0; (void)a1; (void)a2;
            if (!ok) continue;
            ++calibrated;
            t0 = std::min(t0, s.time);
            t1 = std::max(t1, s.time);
            for (int i = 0; i < 90; ++i) {
                const double cross = -49.4444 + i * 1.1111;   // first view left of the track
                const double t = s.time + i * 0.0185;
                double la = 0, lo = 0;
                if (!geo.locate(t, cross, 0.0, &la, &lo)) continue;
                float v[5];
                bool any = false;
                for (int c = 0; c < 5; ++c) {
                    const double C = s.ch[std::size_t(c)][std::size_t(i)];
                    const auto& q = coef[std::size_t(c)];
                    const double bt = C > 0 ? invPlanck(q[0] + q[1] * C + q[2] * C * C, k.wavenumber[c]) : std::nan("");
                    v[c] = (bt > 100 && bt < 350) ? float(bt) : std::nanf("");
                    any = any || std::isfinite(v[c]);
                }
                if (!any) continue;
                const double r = std::max(geo.groundSpacingKm(t, cross, 1.1111), 9.0) * 1.15;
                g.add(la, lo, v, float(r));
            }
        }
        if (calibrated >= 4 && g.build(0.1)) {
            const QString det = QStringLiteral("%1 MHS · %2 · calibrated with the scan's space and warm-load views · TLE %3")
                .arg(sat, when(t0, t1), geo.tleName());
            const char* names[5] = {"MHS_89GHz", "MHS_157GHz", "MHS_183p3pm1GHz", "MHS_183p3pm3GHz", "MHS_190p3GHz"};
            const char* titles[5] = {"MHS 89 GHz brightness temperature (surface, rain / ice scattering)",
                                     "MHS 157 GHz brightness temperature (lower troposphere humidity, ice scattering)",
                                     "MHS 183.3 +-1 GHz brightness temperature (upper-tropospheric humidity, ~350 hPa)",
                                     "MHS 183.3 +-3 GHz brightness temperature (mid-tropospheric humidity, ~500 hPa)",
                                     "MHS 190.3 GHz brightness temperature (lower-tropospheric humidity, ~700 hPa)"};
            for (int c = 0; c < 5; ++c)
                writeMap(g, c, QString::fromLatin1(names[c]), QString::fromLatin1(titles[c]), det, 3, range(g, c, 5.0), rampMicrowave,
                         QStringLiteral("K"), true);
            // scattering by ice: 89 - 157 GHz
            GeoGrid si2 = g;
            (void)si2;
            {
                GeoGrid::Legend L;
                L.title = QStringLiteral("MHS 89 - 157 GHz BT difference: strongly positive where ice in clouds scatters 157 GHz (precipitation, convection)");
                L.detail = det;
                L.ramp = rampDiverging;
                L.lowLabel = QStringLiteral("-40 K");
                L.midLabel = QStringLiteral("0");
                L.highLabel = QStringLiteral("+40 K");
                const QImage img = g.render([](const GeoGrid& gg, int x, int y) -> QRgb {
                    const float a = gg.at(x, y, 0), b = gg.at(x, y, 1);
                    if (!std::isfinite(a) || !std::isfinite(b)) return 0;
                    return rampDiverging((a - b + 40.0) / 80.0);
                }, 3, mapsDir_, L);
                const QString p = QDir(dir).filePath(QStringLiteral("MHS_Scattering_89-157_Map.png"));
                if (img.save(p, "PNG")) out << p;
            }
            // deep convection: Hong et al. (2005) tests on the 183 GHz channels
            {
                GeoGrid::Legend L;
                L.title = QStringLiteral("MHS deep convection (Hong et al. 2005): 183+-1, 183+-3 and 190 GHz BT differences above zero");
                L.detail = det;
                const QRgb c0 = qRgb(46, 50, 58), c1 = qRgb(70, 150, 230), c2c = qRgb(240, 200, 50), c3 = qRgb(225, 50, 50);
                L.classes = {{c0, QStringLiteral("none")}, {c1, QStringLiteral("deep convection (190 < 183+-3)")},
                             {c2c, QStringLiteral("stronger (190 < 183+-1)")}, {c3, QStringLiteral("overshooting (all three)")}};
                const QImage img = g.render([=](const GeoGrid& gg, int x, int y) -> QRgb {
                    const float t3 = gg.at(x, y, 2), t4 = gg.at(x, y, 3), t5 = gg.at(x, y, 4);
                    if (!std::isfinite(t3) || !std::isfinite(t4) || !std::isfinite(t5)) return 0;
                    const bool d45 = t4 - t5 > 0, d35 = t3 - t5 > 0, d34 = t3 - t4 > 0;
                    if (d45 && d35 && d34) return c3;
                    if (d45 && d35) return c2c;
                    if (d45) return c1;
                    return c0;
                }, 3, mapsDir_, L);
                const QString p = QDir(dir).filePath(QStringLiteral("MHS_DeepConvection_Map.png"));
                if (img.save(p, "PNG")) out << p;
            }
            notes << QStringLiteral("MHS %1 of %2 scans calibrated").arg(calibrated).arg(n);
        } else {
            notes << QStringLiteral("MHS: too few calibrated scans (%1)").arg(calibrated);
        }
    } else if (!mhs_.empty()) {
        notes << QStringLiteral("MHS: no calibration coefficients for spacecraft %1").arg(scid_);
    }

    // ---------------------------------------------------------------- AMSU-A
    if (si >= 0 && amsu_.size() >= 4) {
        const AmsuCoefs& k = kAmsuCoefs[si];
        const int pll = k.pll ? 1 : 0, b1 = k.a1Backup ? 1 : 0, b2 = k.a2Backup ? 1 : 0;
        GeoGrid g(15);
        double t0 = 1e300, t1 = 0;
        int calibrated = 0;
        for (const auto& s : amsu_) {
            if (s.time <= 0 || !s.a1) continue;
            std::array<std::array<double, 3>, 15> coef{};
            std::array<bool, 15> ok{};
            // A1: instrument temperatures, warm loads of A1-1 and A1-2
            double T1[45];
            for (int i = 0; i < 45; ++i) {
                T1[i] = 0;
                for (int j = 0; j < 4; ++j) T1[i] += k.a1TempCoefs[i][j] * std::pow(double(s.a1Temps[std::size_t(i)] / 2), j);
            }
            double tw11 = 0, tw12 = 0;
            for (int i = 0; i < 5; ++i) { tw11 += T1[35 + i]; tw12 += T1[40 + i]; }
            tw11 /= 5;
            tw12 /= 5;
            const double tinst1 = T1[k.a1SensorId[b1]];
            for (int c = 0; c < 13; ++c) {
                const int ch = c + 2, mod = kAmsuModule[ch];
                const double* ut = mod == 0 ? k.a11UTemps[pll ? 2 : b1] : k.a12UTemps[b1];
                double wc[3] = {k.a1WarmCorr[pll][0][c], k.a1WarmCorr[pll][1][c], k.a1WarmCorr[pll][2][c]};
                double uu[3] = {k.a1U[pll][0][c], k.a1U[pll][1][c], k.a1U[pll][2][c]};
                const double nu = kAmsuWavenumber[ch];
                const double Rw = planck((mod == 0 ? tw11 : tw12) + interp3(ut, wc, tinst1), nu);
                const double Rc = planck(2.73 + k.a1ColdCorr[pll][c], nu);
                const double Cw = s.a1Views[std::size_t(c)][0], Cs = s.a1Views[std::size_t(c)][1];
                if (Cw <= 0 || Cs <= 0 || Cw == Cs || !(tw11 > 250 && tw11 < 330)) continue;
                const double G = (Cw - Cs) / (Rw - Rc), u = interp3(ut, uu, tinst1);
                coef[std::size_t(ch)] = {Rw - Cw / G + u * Cw * Cs / (G * G), 1.0 / G - u * (Cw + Cs) / (G * G), u / (G * G)};
                ok[std::size_t(ch)] = true;
            }
            if (s.a2) {
                double T2[19];
                for (int i = 0; i < 19; ++i) {
                    T2[i] = 0;
                    for (int j = 0; j < 4; ++j) T2[i] += k.a2TempCoefs[i][j] * std::pow(double(s.a2Temps[std::size_t(i)] / 2), j);
                }
                double tw = 0;
                for (int i = 0; i < 7; ++i) tw += T2[12 + i];
                tw /= 7;
                const double tinst2 = T2[k.a2SensorId[b2]];
                for (int c = 0; c < 2; ++c) {
                    double wc[3] = {k.a2WarmCorr[0][c], k.a2WarmCorr[1][c], k.a2WarmCorr[2][c]};
                    double uu[3] = {k.a2U[0][c], k.a2U[1][c], k.a2U[2][c]};
                    const double nu = kAmsuWavenumber[c];
                    const double Rw = planck(tw + interp3(k.a2UTemps[b2], wc, tinst2), nu);
                    const double Rc = planck(2.73 + k.a2ColdCorr[c], nu);
                    const double Cw = s.a2Views[std::size_t(c)][0], Cs = s.a2Views[std::size_t(c)][1];
                    if (Cw <= 0 || Cs <= 0 || Cw == Cs || !(tw > 250 && tw < 330)) continue;
                    const double G = (Cw - Cs) / (Rw - Rc), u = interp3(k.a2UTemps[b2], uu, tinst2);
                    coef[std::size_t(c)] = {Rw - Cw / G + u * Cw * Cs / (G * G), 1.0 / G - u * (Cw + Cs) / (G * G), u / (G * G)};
                    ok[std::size_t(c)] = true;
                }
            }
            if (!ok[2]) continue;
            ++calibrated;
            t0 = std::min(t0, s.time);
            t1 = std::max(t1, s.time);
            for (int i = 0; i < 30; ++i) {
                const double cross = -48.3333 + i * 3.3333;
                const double t = s.time + i * 0.2;
                double la = 0, lo = 0;
                if (!geo.locate(t, cross, 0.0, &la, &lo)) continue;
                float v[15];
                for (int c = 0; c < 15; ++c) {
                    const double C = s.ch[std::size_t(c)][std::size_t(i)];
                    const auto& q = coef[std::size_t(c)];
                    const double bt = ok[std::size_t(c)] && C > 0 ? invPlanck(q[0] + q[1] * C + q[2] * C * C, kAmsuWavenumber[c]) : std::nan("");
                    v[c] = (bt > 80 && bt < 350) ? float(bt) : std::nanf("");
                }
                const double r = std::max(geo.groundSpacingKm(t, cross, 3.3333), 27.0) * 1.15;
                g.add(la, lo, v, float(r));
            }
        }
        if (calibrated >= 3 && g.build(0.2)) {
            const QString det = QStringLiteral("%1 AMSU-A · %2 · calibrated with the scan's space and warm-load views · limb not corrected · TLE %3")
                .arg(sat, when(t0, t1), geo.tleName());
            struct P { int ch; const char* name; const char* title; };
            const P list[] = {
                {0, "AMSU_CH01_23p8GHz", "AMSU-A ch1 23.8 GHz BT: water vapour and cloud liquid water over the sea, surface over land"},
                {1, "AMSU_CH02_31p4GHz", "AMSU-A ch2 31.4 GHz BT: cloud liquid water and rain over the sea, surface over land"},
                {2, "AMSU_CH03_50p3GHz", "AMSU-A ch3 50.3 GHz BT: surface and lowest troposphere"},
                {3, "AMSU_CH04_52p8GHz", "AMSU-A ch4 52.8 GHz BT: lower troposphere temperature (~900 hPa)"},
                {4, "AMSU_CH05_53p6GHz", "AMSU-A ch5 53.6 GHz BT: lower-mid troposphere temperature (~700 hPa)"},
                {5, "AMSU_CH06_54p4GHz", "AMSU-A ch6 54.4 GHz BT: mid troposphere temperature (~400 hPa)"},
                {6, "AMSU_CH07_54p9GHz", "AMSU-A ch7 54.9 GHz BT: upper troposphere temperature (~250 hPa)"},
                {7, "AMSU_CH08_55p5GHz", "AMSU-A ch8 55.5 GHz BT: tropopause temperature (~180 hPa)"},
                {8, "AMSU_CH09_57p29GHz", "AMSU-A ch9 57.29 GHz BT: lower stratosphere temperature (~90 hPa)"},
                {14, "AMSU_CH15_89GHz", "AMSU-A ch15 89 GHz BT: surface, rain and ice scattering"}};
            QStringList failed;
            for (const auto& p : list) {
                // quality gate: a channel whose values leave the physical range (a noisy or failed
                // channel on this spacecraft) is not drawn
                const bool window = p.ch <= 2 || p.ch == 14;
                const double lo = window ? 100.0 : 180.0, hi = window ? 320.0 : 300.0;
                std::size_t all = 0, bad = 0;
                for (int y = 0; y < g.height(); ++y)
                    for (int x = 0; x < g.width(); ++x) {
                        const float v = g.at(x, y, p.ch);
                        if (!std::isfinite(v)) continue;
                        ++all;
                        if (v < lo || v > hi) ++bad;
                    }
                if (all == 0 || bad * 50 > all) { failed << QStringLiteral("ch%1").arg(p.ch + 1); continue; }
                writeMap(g, p.ch, QString::fromLatin1(p.name), QString::fromLatin1(p.title), det, 5, range(g, p.ch, 2.0), rampMicrowave, QStringLiteral("K"), true);
            }
            if (!failed.isEmpty()) notes << QStringLiteral("AMSU-A %1 left out (values outside the physical range: noisy channel)").arg(failed.join(QStringLiteral(" ")));
            notes << QStringLiteral("AMSU-A %1 of %2 scans calibrated").arg(calibrated).arg(amsu_.size());
        } else {
            notes << QStringLiteral("AMSU-A: too few calibrated scans (%1)").arg(calibrated);
        }
    }

    // ---------------------------------------------------------------- IASI imager
    if (!iisScans_.empty()) {
        GeoGrid g(1);
        const double um = 11.4, fs = 3.3333, ps = fs / 64.0;
        const double c1 = 1.191042e8, c2 = 1.4387752e4;
        auto rad = [&](double t) { return c1 / (std::pow(um, 5.0) * (std::exp(c2 / (um * t)) - 1.0)); };
        auto tem = [&](double l) { return l > 0 ? c2 / (um * std::log(1.0 + c1 / (std::pow(um, 5.0) * l))) : std::nan(""); };
        double t0 = 1e300, t1 = 0;
        for (std::size_t s = 0; s < iisScans_.size(); ++s) {
            const auto& f = iisScans_[s];
            const double tbb = iisBbt_[s] > 200 ? iisBbt_[s] : 290.0;
            const double lbb = rad(tbb);
            const auto& ft = s < iisFieldTimes_.size() ? iisFieldTimes_[s] : std::array<double, 36>{};
            double scanT = 0;
            for (double x : ft) if (x > 0) { scanT = x; break; }
            if (scanT <= 0) continue;
            for (int field = 1; field <= 30; ++field) {
                double t = ft[std::size_t(field - 1)];
                if (t <= 0) t = scanT + (field - 1) * 0.217;
                t0 = std::min(t0, t);
                t1 = std::max(t1, t);
                for (int i = 0; i < 64; i += 1)
                    for (int y = 0; y < 64; y += 1) {
                        const std::size_t col = std::size_t(2303 - ((field - 1) * 64 + y));
                        const double c = f[std::size_t(i) * 2304 + col];
                        // cold space: fields 35, 36; blackbody: fields 32, 33 (same detector pixel)
                        auto px = [&](int fld) { return double(f[std::size_t(i) * 2304 + std::size_t(2303 - ((fld - 1) * 64 + y))]); };
                        const double cold = 0.5 * (px(36) + px(35)), warm = 0.5 * (px(33) + px(32));
                        if (c <= 0 || cold <= 0 || warm <= 0 || std::fabs(warm - cold) < 1) continue;
                        const float bt = float(tem((c - cold) / (warm - cold) * lbb));
                        if (!(bt > 150 && bt < 345)) continue;
                        const double cross = -48.3333 + (field - 1) * fs + (y - 31.5) * ps, along = (i - 31.5) * ps;
                        double la = 0, lo = 0;
                        if (!geo.locate(t, cross, along, &la, &lo)) continue;
                        g.add(la, lo, &bt, float(std::max(geo.groundSpacingKm(t, cross, ps), 0.8) * 1.6));
                    }
            }
        }
        if (g.build(0.02)) {
            const QString det = QStringLiteral("%1 IASI integrated imager · %2 · 10.3-12.5 um, calibrated with the space and blackbody views · TLE %3")
                .arg(sat, when(t0, t1), geo.tleName());
            GeoGrid::Legend L;
            L.title = QStringLiteral("IASI imager brightness temperature (cloud tops cold, coloured below -30 C)");
            L.detail = det;
            L.ramp = [](double t) { return rampCloudTop(t); };
            L.lowLabel = QStringLiteral("330 K");
            L.midLabel = QStringLiteral("255 K");
            L.highLabel = QStringLiteral("180 K");
            const QImage img = g.render([](const GeoGrid& gg, int x, int y) -> QRgb {
                const float v = gg.at(x, y, 0);
                if (!std::isfinite(v)) return 0;
                return rampCloudTop((330.0 - v) / 150.0);
            }, 1, mapsDir_, L);
            const QString p = QDir(dir).filePath(QStringLiteral("IASI_IIS_Map.png"));
            if (img.save(p, "PNG")) out << p;
            const QString q = QDir(dir).filePath(QStringLiteral("IASI_IIS_Map_TBB_0p01K.png"));
            if (g.saveRaw16(0, 100.0, 0.0, q)) out << q;
            notes << QStringLiteral("IASI imager map %1 x %2").arg(g.width()).arg(g.height());
        }
    }
    mapsLog_ = QStringLiteral("maps: ") + notes.join(QStringLiteral(", "));
    return out;
}

// =============================================================================================
// METEOR-M MTVZA-GY map products.
//
// MTVZA scans a cone: every look is the same angle off nadir, swept in azimuth about the local
// vertical behind the satellite, so a scan lands on the ground as an arc ~1200 km aft of the
// sub-satellite point. 200 samples per scan cover 144 deg of azimuth (0.72 deg per sample). Cone
// angle and azimuth offset per satellite as in SatDump's projection settings (normal_single_line,
// rotate_yaw); the scan time is the MSU-MR line time when the scan ended, minus 10 s. On the
// METEOR-M N2-3 pass of 2026-11-22 01:47 UTC the coast of Korea and Primorye falls on the
// sea / land edge of the window channels with these values.
void MtvzaReader::geometry(int serial, double* coneDeg, double* azimuth0Deg) {
    // azimuth of sample x = azimuth0 - 0.72 x, measured from straight aft towards the left
    switch (serial) {
    case 4: *coneDeg = 51.0; *azimuth0Deg = 66.5; break;    // N2-4
    case 2: *coneDeg = 53.3; *azimuth0Deg = 9.0; break;     // N2-2
    default: *coneDeg = 53.3; *azimuth0Deg = 66.0; break;   // N2-3, and unknown
    }
}

QStringList MtvzaReader::saveMaps(const QString& dir, const std::vector<std::array<std::array<float, 200>, 30>>& v) const {
    QStringList out;
    SwathGeo geo(tleName_, tle1_, tle2_);
    if (!geo.valid()) { mapsLog_ = QStringLiteral("maps: no TLE for this METEOR"); return out; }
    const int h = int(v.size());
    // scan times: the MSU-MR clock; gaps bridged at the 2.41 s scan period
    constexpr double kScan = 2.41, kOffset = -10.0;
    // The MSU-MR line time comes over the link without error protection: a hit time code puts a scan
    // hours away. A scan's time is kept only when it agrees within 3 s with the median of its
    // neighbours (up to 6 either side, each carried along at the scan period); the others are
    // bridged from the nearest good scan.
    std::vector<double> raw(std::size_t(h), 0.0);
    for (int y = 0; y < h && y < int(scanTimes_.size()); ++y) raw[std::size_t(y)] = scanTimes_[std::size_t(y)];
    std::vector<char> good(std::size_t(h), 0);
    int nGood = 0;
    for (int y = 0; y < h; ++y) {
        if (raw[std::size_t(y)] <= 0) continue;
        std::vector<double> est;
        for (int j = std::max(0, y - 6); j <= std::min(h - 1, y + 6); ++j)
            if (j != y && raw[std::size_t(j)] > 0) est.push_back(raw[std::size_t(j)] + (y - j) * kScan);
        if (est.size() < 3) continue;
        std::nth_element(est.begin(), est.begin() + long(est.size() / 2), est.end());
        if (std::fabs(raw[std::size_t(y)] - est[est.size() / 2]) <= 3.0) { good[std::size_t(y)] = 1; ++nGood; }
    }
    if (nGood == 0) { mapsLog_ = QStringLiteral("maps: no usable MSU-MR time"); return out; }
    std::vector<double> t(std::size_t(h), 0.0);
    for (int y = 0; y < h; ++y) {
        if (good[std::size_t(y)]) { t[std::size_t(y)] = raw[std::size_t(y)] + kOffset; continue; }
        int best = -1;
        for (int d = 1; d < h && best < 0; ++d) {
            if (y - d >= 0 && good[std::size_t(y - d)]) best = y - d;
            else if (y + d < h && good[std::size_t(y + d)]) best = y + d;
        }
        t[std::size_t(y)] = raw[std::size_t(best)] + kOffset + (y - best) * kScan;
    }
    const int badTimes = h - nGood;
    double cone = 0, az0 = 0;
    geometry(serial_, &cone, &az0);
    const double ct = cone * M_PI / 180.0;
    auto lookAt = [&](double ty, double x, double* la, double* lo) {
        const double psi = (az0 - 0.72 * x) * M_PI / 180.0;
        const double dAlong = -std::sin(ct) * std::cos(psi), dCross = -std::sin(ct) * std::sin(psi), dDown = std::cos(ct);
        return geo.locate(ty, std::atan2(dCross, dDown) * 180.0 / M_PI, std::atan2(dAlong, dDown) * 180.0 / M_PI, la, lo);
    };
    auto distKm = [](double a1, double o1, double a2, double o2) {
        const double k = M_PI / 180.0;
        const double s = std::sin((a2 - a1) * k / 2), c = std::sin((o2 - o1) * k / 2);
        return 2.0 * 6371.0 * std::asin(std::sqrt(s * s + std::cos(a1 * k) * std::cos(a2 * k) * c * c));
    };
    // ground location and footprint radius of every unique sample, per sample spacing (1, 2, 4)
    struct Pt { float lat = 0, lon = 0, r = 0; bool ok = false; };
    std::map<int, std::vector<Pt>> pts;
    for (int st : {1, 2, 4}) {
        auto& P = pts[st];
        P.assign(std::size_t(h) * std::size_t(200 / st), Pt{});
        for (int y = 0; y < h; ++y)
            for (int j = 0; j < 200 / st; ++j) {
                const double x = j * st + (st - 1) / 2.0;
                double la, lo, la2, lo2, la3, lo3;
                if (!lookAt(t[std::size_t(y)], x, &la, &lo)) continue;
                double r = 8.0;
                if (lookAt(t[std::size_t(y)], x + st, &la2, &lo2)) r = std::max(r, distKm(la, lo, la2, lo2));
                if (lookAt(t[std::size_t(y)] + kScan, x, &la3, &lo3)) r = std::max(r, distKm(la, lo, la3, lo3));
                P[std::size_t(y) * std::size_t(200 / st) + std::size_t(j)] = Pt{float(la), float(lo), float(0.6 * r), true};
            }
    }
    const QString sat = serial_ == 3 ? QStringLiteral("METEOR-M N2-3") : serial_ == 4 ? QStringLiteral("METEOR-M N2-4")
                      : serial_ == 2 ? QStringLiteral("METEOR-M N2-2") : QStringLiteral("METEOR-M");
    const QString when = QDateTime::fromMSecsSinceEpoch(qint64(t.front() * 1000), QTimeZone::UTC).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                       + QStringLiteral(" - ") + QDateTime::fromMSecsSinceEpoch(qint64(t.back() * 1000), QTimeZone::UTC).toString(QStringLiteral("HH:mm:ss 'UTC'"));
    constexpr double kRes = 0.1;
    constexpr int kZoom = 4;
    int maps = 0;
    // one map per channel: the relative brightness (sign-flipped counts), stretched over the pass
    for (int c = 0; c < 30; ++c) {
        const int st = sampleStep(c);
        const auto& P = pts[st];
        GeoGrid g(1);
        for (int y = 0; y < h; ++y)
            for (int j = 0; j < 200 / st; ++j) {
                const Pt& p = P[std::size_t(y) * std::size_t(200 / st) + std::size_t(j)];
                const float val = v[std::size_t(y)][std::size_t(c)][std::size_t(j * st)];
                if (p.ok && std::isfinite(val)) g.add(p.lat, p.lon, &val, p.r);
            }
        if (g.samples() < 100 || !g.build(kRes)) continue;
        std::vector<float> all;
        for (int y = 0; y < g.height(); ++y) for (int x = 0; x < g.width(); ++x) { const float a = g.at(x, y, 0); if (std::isfinite(a)) all.push_back(a); }
        if (all.size() < 50) continue;
        std::sort(all.begin(), all.end());
        const float lo = all[all.size() / 100], hi = std::max(all[all.size() * 99 / 100], lo + 1.0f);
        GeoGrid::Legend L;
        L.title = QStringLiteral("%1 MTVZA-GY channel %2 (%3 resolution)  |  %4")
                      .arg(sat).arg(c + 1).arg(st == 1 ? QStringLiteral("full") : st == 2 ? QStringLiteral("medium") : QStringLiteral("low"), when);
        L.detail = QStringLiteral("relative brightness: raw counts with the sign flipped (higher = warmer), stretched 1-99 % over the pass; uncalibrated, not kelvin");
        L.ramp = rampMicrowave;
        L.lowLabel = QStringLiteral("colder");
        L.highLabel = QStringLiteral("warmer");
        const QImage img = g.render([&](const GeoGrid& gg, int x, int y) -> QRgb {
            const float a = gg.at(x, y, 0);
            return std::isfinite(a) ? rampMicrowave(std::clamp((a - lo) / (hi - lo), 0.0f, 1.0f)) : 0;
        }, kZoom, mapsDir_, L);
        const QString path = QDir(dir).filePath(QStringLiteral("MTVZA_CH%1_Map.png").arg(c + 1, 2, 10, QLatin1Char('0')));
        if (img.save(path, "PNG")) { out << path; ++maps; }
    }
    // colour composites (SatDump's MTVZA presets, on the raw counts as there: sea and lakes read
    // high, so they come out light)
    struct Rgb { const char* name; int r, g, b; const char* what; };
    static const Rgb kRgb[] = {
        {"MTVZA_RGB_SoilMoisture_Map", 14, 13, 12, "soil moisture: channels 14 / 13 / 12 (48 GHz H, 48 GHz V, 42 GHz H)"},
        {"MTVZA_RGB_Vegetation_Map", 16, 15, 17, "vegetation and snow: channels 16 / 15 / 17 (53.3, 52.8, 53.8 GHz)"},
        {"MTVZA_RGB_MicrowaveAirmass_Map", 30, 29, 28, "microwave airmass: channels 30 / 29 / 28 (183 GHz water vapour)"},
    };
    for (const Rgb& k : kRgb) {
        const int st = 2;   // all medium resolution
        const auto& P = pts[st];
        GeoGrid g(3);
        for (int y = 0; y < h; ++y)
            for (int j = 0; j < 100; ++j) {
                const Pt& p = P[std::size_t(y) * 100 + std::size_t(j)];
                const auto& s = v[std::size_t(y)];
                const float val[3] = {-s[std::size_t(k.r - 1)][std::size_t(j * 2)], -s[std::size_t(k.g - 1)][std::size_t(j * 2)], -s[std::size_t(k.b - 1)][std::size_t(j * 2)]};
                if (p.ok && std::isfinite(val[0]) && std::isfinite(val[1]) && std::isfinite(val[2])) g.add(p.lat, p.lon, val, p.r);
            }
        if (g.samples() < 100 || !g.build(kRes)) continue;
        float lo[3], hi[3];
        bool okAll = true;
        for (int b = 0; b < 3; ++b) {
            std::vector<float> all;
            for (int y = 0; y < g.height(); ++y) for (int x = 0; x < g.width(); ++x) { const float a = g.at(x, y, b); if (std::isfinite(a)) all.push_back(a); }
            if (all.size() < 50) { okAll = false; break; }
            std::sort(all.begin(), all.end());
            lo[b] = all[all.size() / 100]; hi[b] = std::max(all[all.size() * 99 / 100], lo[b] + 1.0f);
        }
        if (!okAll) continue;
        GeoGrid::Legend L;
        L.title = QStringLiteral("%1 MTVZA-GY  |  %2").arg(sat, when);
        L.detail = QString::fromLatin1(k.what) + QStringLiteral("; raw counts, each stretched 1-99 %; uncalibrated");
        const QImage img = g.render([&](const GeoGrid& gg, int x, int y) -> QRgb {
            const float a = gg.at(x, y, 0);
            if (!std::isfinite(a)) return 0;
            int c[3];
            for (int b = 0; b < 3; ++b) c[b] = int(std::lround(255.0f * std::clamp((gg.at(x, y, b) - lo[b]) / (hi[b] - lo[b]), 0.0f, 1.0f)));
            return qRgb(c[0], c[1], c[2]) | 0xFF000000u;
        }, kZoom, mapsDir_, L);
        const QString path = QDir(dir).filePath(QString::fromLatin1(k.name) + QStringLiteral(".png"));
        if (img.save(path, "PNG")) { out << path; ++maps; }
    }
    mapsLog_ = QStringLiteral("maps: %1 (%2, cone %3 deg; %4 of %5 scan times bridged)").arg(maps).arg(QString::fromStdString(tleName_)).arg(cone).arg(badTimes).arg(h);
    return out;
}

}  // namespace nadirstation::polar
