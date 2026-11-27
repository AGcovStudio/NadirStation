#include "passpredict.h"

#include "CoordGeodetic.h"
#include "CoordTopocentric.h"
#include "DateTime.h"
#include "Eci.h"
#include "Observer.h"
#include "SGP4.h"
#include "SolarPosition.h"
#include "Tle.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>

namespace nadirstation::pass {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = 180.0 / kPi;
constexpr double kEarthKm = 6378.137;

// libsgp4 counts ticks from 0001-01-01; Unix time is converted through the tick count of 1970-01-01
// and the number of ticks per second, both read from the library rather than assumed.
struct TickBase {
    std::int64_t unixTicks;
    double ticksPerSecond;
    TickBase() {
        const libsgp4::DateTime a(1970, 1, 1, 0, 0, 0);
        const libsgp4::DateTime b(1970, 1, 2, 0, 0, 0);
        unixTicks = a.Ticks();
        ticksPerSecond = double(b.Ticks() - a.Ticks()) / 86400.0;
    }
};
const TickBase& ticks() {
    static const TickBase t;
    return t;
}

double toUnix(const libsgp4::DateTime& dt) { return double(dt.Ticks() - ticks().unixTicks) / ticks().ticksPerSecond; }

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

bool isLine(const std::string& s, char n) {
    return s.size() >= 69 && s[0] == n && s[1] == ' ';
}

// The range-rate member is `rangeRate` in current libsgp4 and `range_rate` in older copies.
template <class T>
auto rangeRateOf(const T& t, int) -> decltype(t.rangeRate) { return t.rangeRate; }
template <class T>
auto rangeRateOf(const T& t, long) -> decltype(t.range_rate) { return t.range_rate; }

double wrap360(double a) {
    a = std::fmod(a, 360.0);
    return a < 0 ? a + 360.0 : a;
}

}  // namespace

// ------------------------------------------------------------------------------------------------

std::vector<TleEntry> parseTle(const std::string& text, int* skipped) {
    std::vector<TleEntry> out;
    int bad = 0;
    std::istringstream in(text);
    std::string line, name, l1;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (isLine(line, '1')) {
            l1 = line;
            continue;
        }
        if (isLine(line, '2') && !l1.empty()) {
            TleEntry e;
            e.line1 = l1.substr(0, 69);
            e.line2 = line.substr(0, 69);
            e.name = name;
            try {
                const libsgp4::Tle t(e.name.empty() ? std::string("SAT") : e.name, e.line1, e.line2);
                e.norad = int(t.NoradNumber());
                if (e.name.empty()) e.name = "NORAD " + std::to_string(e.norad);
                out.push_back(std::move(e));
            } catch (...) {
                ++bad;
            }
            l1.clear();
            name.clear();
            continue;
        }
        // A name line (Space-Track's 3-line format writes it as "0 NAME").
        l1.clear();
        name = (line.size() > 2 && line[0] == '0' && line[1] == ' ') ? trim(line.substr(2)) : line;
    }
    if (skipped) *skipped = bad;
    return out;
}

// ------------------------------------------------------------------------------------------------

struct Tracker::Impl {
    std::unique_ptr<libsgp4::Tle> tle;
    std::unique_ptr<libsgp4::SGP4> sgp4;
    // libsgp4's Observer and SolarPosition update cached state in their const-looking calls.
    mutable std::unique_ptr<libsgp4::Observer> obs;
    mutable Station obsAt{1e9, 1e9, 1e9};
    mutable libsgp4::SolarPosition sun;

    libsgp4::Observer& observer(const Station& st) const {
        if (!obs || st.latDeg != obsAt.latDeg || st.lonDeg != obsAt.lonDeg || st.altM != obsAt.altM) {
            obs = std::make_unique<libsgp4::Observer>(st.latDeg, st.lonDeg, st.altM / 1000.0);
            obsAt = st;
        }
        return *obs;
    }
};

Tracker::Tracker(const TleEntry& tle) : d_(std::make_unique<Impl>()), tle_(tle) {
    try {
        d_->tle = std::make_unique<libsgp4::Tle>(tle.name.empty() ? std::string("SAT") : tle.name, tle.line1, tle.line2);
        d_->sgp4 = std::make_unique<libsgp4::SGP4>(*d_->tle);
        const double mm = d_->tle->MeanMotion();   // revolutions per day
        periodMin_ = mm > 0 ? 1440.0 / mm : 0.0;
        epochUnix_ = toUnix(d_->tle->Epoch());
        valid_ = periodMin_ > 0;
        if (!valid_) error_ = "mean motion is zero";
    } catch (const std::exception& e) {
        error_ = e.what();
    } catch (...) {
        error_ = "TLE could not be parsed";
    }
}

Tracker::~Tracker() = default;

double Tracker::elevation(const Station& st, double t) const {
    if (!valid_) return std::numeric_limits<double>::quiet_NaN();
    try {
        const libsgp4::Eci eci = d_->sgp4->FindPosition((t - epochUnix_) / 60.0);
        return d_->observer(st).GetLookAngle(eci).elevation * kDeg;
    } catch (...) {
        return std::numeric_limits<double>::quiet_NaN();
    }
}

Look Tracker::look(const Station& st, double t, bool withSun) const {
    Look r;
    if (!valid_) return r;
    try {
        const libsgp4::Eci eci = d_->sgp4->FindPosition((t - epochUnix_) / 60.0);
        libsgp4::Observer& o = d_->observer(st);
        const libsgp4::CoordTopocentric topo = o.GetLookAngle(eci);
        const libsgp4::CoordGeodetic geo = eci.ToGeodetic();
        r.azDeg = wrap360(topo.azimuth * kDeg);
        r.elDeg = topo.elevation * kDeg;
        r.rangeKm = topo.range;
        r.rangeRateKmS = rangeRateOf(topo, 0);
        r.latDeg = geo.latitude * kDeg;
        r.lonDeg = geo.longitude * kDeg;
        if (r.lonDeg > 180.0) r.lonDeg -= 360.0;
        if (r.lonDeg < -180.0) r.lonDeg += 360.0;
        r.altKm = geo.altitude;
        if (withSun) {
            const libsgp4::Eci s = d_->sun.FindPosition(eci.GetDateTime());
            const libsgp4::Vector sp = s.Position();
            const libsgp4::Vector p = eci.Position();
            const double sn = std::sqrt(sp.x * sp.x + sp.y * sp.y + sp.z * sp.z);
            const double ux = sp.x / sn, uy = sp.y / sn, uz = sp.z / sn;
            const double along = p.x * ux + p.y * uy + p.z * uz;
            const double px = p.x - along * ux, py = p.y - along * uy, pz = p.z - along * uz;
            const double perp = std::sqrt(px * px + py * py + pz * pz);
            r.sunlit = !(along < 0.0 && perp < kEarthKm);
            r.sunElDeg = o.GetLookAngle(s).elevation * kDeg;
        }
        r.ok = std::isfinite(r.elDeg) && std::isfinite(r.azDeg);
    } catch (...) {
        r.ok = false;
    }
    return r;
}

bool Tracker::subPoint(double t, double* latDeg, double* lonDeg, double* altKm) const {
    if (!valid_) return false;
    try {
        const libsgp4::CoordGeodetic geo = d_->sgp4->FindPosition((t - epochUnix_) / 60.0).ToGeodetic();
        double lon = geo.longitude * kDeg;
        if (lon > 180.0) lon -= 360.0;
        if (lon < -180.0) lon += 360.0;
        if (latDeg) *latDeg = geo.latitude * kDeg;
        if (lonDeg) *lonDeg = lon;
        if (altKm) *altKm = geo.altitude;
        return std::isfinite(geo.latitude);
    } catch (...) {
        return false;
    }
}

// ------------------------------------------------------------------------------------------------

std::vector<Pass> predictOne(Tracker& tr, int satIndex, const Station& st, const PredictOptions& opt,
                             const std::atomic<bool>* cancel) {
    std::vector<Pass> out;
    if (!tr.valid()) return out;
    const double h = opt.horizonDeg;
    const double t0 = opt.startUnix;
    const double t1 = opt.startUnix + std::max(0.01, opt.days) * 86400.0;
    // Step: 1/360 of the period, 10..60 s. A low-orbit pass lasts several minutes even when it only
    // grazes the horizon, so this finds every pass that gets more than a fraction of a degree up.
    const double step = std::clamp(tr.periodMinutes() * 60.0 / 360.0, 10.0, 60.0);
    auto el = [&](double t) { return tr.elevation(st, t) - h; };

    // Time where el() crosses zero in [a, b] (el(a) and el(b) of opposite sign).
    auto crossing = [&](double a, double ea, double b) {
        for (int i = 0; i < 40 && b - a > 0.2; ++i) {
            const double m = 0.5 * (a + b);
            const double em = el(m);
            if (!std::isfinite(em)) break;
            if ((em >= 0) == (ea >= 0)) { a = m; ea = em; } else b = m;
        }
        return 0.5 * (a + b);
    };

    auto finish = [&](double aos, double los, double tSample, bool before, bool after) {
        // Golden-section search for the highest point around the best sample.
        double a = std::max(aos, tSample - step), b = std::min(los, tSample + step);
        const double g = 0.6180339887498949;
        double c = b - g * (b - a), dd = a + g * (b - a);
        double fc = el(c), fd = el(dd);
        for (int i = 0; i < 40 && b - a > 0.5; ++i) {
            if (fc > fd) { b = dd; dd = c; fd = fc; c = b - g * (b - a); fc = el(c); }
            else { a = c; c = dd; fc = fd; dd = a + g * (b - a); fd = el(dd); }
        }
        double tca = 0.5 * (a + b);
        // The window edges can be the highest point of a clipped pass.
        double best = el(tca);
        for (double te : {aos, los}) {
            const double e = el(te);
            if (std::isfinite(e) && e > best) { best = e; tca = te; }
        }
        const Look la = tr.look(st, tca, false);
        if (!la.ok || la.elDeg < opt.minPeakDeg) return;
        Pass p;
        p.sat = satIndex;
        p.name = tr.tle().name;
        p.norad = tr.tle().norad;
        p.aos = aos;
        p.los = los;
        p.tca = tca;
        p.maxElDeg = la.elDeg;
        p.tcaAzDeg = la.azDeg;
        p.aosAzDeg = tr.look(st, aos).azDeg;
        p.losAzDeg = tr.look(st, los).azDeg;
        p.startsBeforeWindow = before;
        p.endsAfterWindow = after;
        p.alwaysUp = before && after;
        const int n = std::max(2, opt.trackPoints);
        p.track.reserve(size_t(n));
        for (int i = 0; i < n; ++i) {
            const double t = aos + (los - aos) * double(i) / double(n - 1);
            const bool sunCheck = (i % 6) == 0 || i == n - 1;
            const Look l = tr.look(st, t, sunCheck);
            if (!l.ok) continue;
            p.track.push_back({t, l.azDeg, l.elDeg});
            if (sunCheck && l.sunlit && l.sunElDeg < -6.0 && l.elDeg >= h) p.visible = true;
        }
        out.push_back(std::move(p));
    };

    double prevT = t0, prevE = el(t0);
    if (!std::isfinite(prevE)) return out;
    bool up = prevE >= 0;
    double aos = t0, bestT = t0, bestE = prevE;
    bool aosBefore = up;
    int n = 0;
    for (double t = t0 + step;; t += step) {
        if (t > t1) t = t1;
        if (cancel && (++n & 255) == 0 && cancel->load(std::memory_order_relaxed)) return out;
        const double e = el(t);
        if (!std::isfinite(e)) break;   // decayed / propagation failed
        if (!up && e >= 0) {
            aos = crossing(prevT, prevE, t);
            aosBefore = false;
            up = true;
            bestT = t;
            bestE = e;
        } else if (up && e > bestE) {
            bestT = t;
            bestE = e;
        }
        if (up && e < 0) {
            const double los = crossing(prevT, prevE, t);
            finish(aos, los, bestT, aosBefore, false);
            up = false;
            bestE = -1e9;
        }
        prevT = t;
        prevE = e;
        if (t >= t1) break;
    }
    if (up) finish(aos, t1, bestT, aosBefore, true);
    return out;
}

std::vector<Pass> predictAll(const std::vector<TleEntry>& sats, const Station& st, const PredictOptions& opt,
                             std::atomic<int>* done, const std::atomic<bool>* cancel, int threads) {
    if (threads <= 0) threads = int(std::max(1u, std::thread::hardware_concurrency()));
    threads = std::max(1, std::min<int>(threads, int(sats.size())));
    std::atomic<int> next{0};
    std::mutex mu;
    std::vector<Pass> all;
    auto work = [&]() {
        std::vector<Pass> mine;
        for (;;) {
            if (cancel && cancel->load()) break;
            const int i = next.fetch_add(1);
            if (i >= int(sats.size())) break;
            Tracker tr(sats[size_t(i)]);
            std::vector<Pass> p = predictOne(tr, i, st, opt, cancel);
            mine.insert(mine.end(), std::make_move_iterator(p.begin()), std::make_move_iterator(p.end()));
            if (done) done->fetch_add(1);
        }
        std::lock_guard<std::mutex> lk(mu);
        all.insert(all.end(), std::make_move_iterator(mine.begin()), std::make_move_iterator(mine.end()));
    };
    std::vector<std::thread> pool;
    for (int k = 1; k < threads; ++k) pool.emplace_back(work);
    work();
    for (auto& th : pool) th.join();
    std::sort(all.begin(), all.end(), [](const Pass& a, const Pass& b) {
        return a.aos != b.aos ? a.aos < b.aos : a.norad < b.norad;
    });
    return all;
}

double footprintRadiusDeg(double altKm, double horizonDeg) {
    if (altKm <= 0) return 0.0;
    const double h = horizonDeg / kDeg;
    const double c = kEarthKm * std::cos(h) / (kEarthKm + altKm);
    return (std::acos(std::clamp(c, -1.0, 1.0)) - h) * kDeg;
}

}  // namespace nadirstation::pass
