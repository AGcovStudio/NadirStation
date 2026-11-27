#include "swcore.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace nadirstation::spaceweather {
namespace {
double median(std::vector<double> v) {
    if (v.empty()) return std::nan("");
    const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    double m = *mid;
    if (v.size() % 2 == 0) {
        const double lower = *std::max_element(v.begin(), mid);
        m = 0.5 * (m + lower);
    }
    return m;
}

void dropOutliers(Series& s) {
    if (s.size() < 10) return;
    std::vector<double> v;
    v.reserve(s.size());
    for (const auto& p : s) v.push_back(p.second);
    const double med = median(v);
    for (auto& x : v) x = std::fabs(x - med);
    const double mad = std::max(1.0, median(v));
    s.erase(std::remove_if(s.begin(), s.end(), [&](const std::pair<double, double>& p) {
        return std::fabs(p.second - med) > 25.0 * mad;
    }), s.end());
}
}  // namespace

Level worse(Level a, Level b) {
    return static_cast<int>(a) >= static_cast<int>(b) ? a : b;
}

const char* levelName(Level l, Mode m) {
    switch (l) {
    case Level::Unknown: return "Building baseline";
    case Level::Quiet: return m == Mode::Decrease ? "Normal" : "Quiet";
    case Level::Active: return m == Mode::Ratio ? "Elevated" : m == Mode::Decrease ? "Decreasing" : "Disturbed";
    case Level::Elevated: return m == Mode::Ratio ? "Enhanced" : m == Mode::Decrease ? "Clearly decreasing" : "Moderately disturbed";
    case Level::Strong: return m == Mode::Ratio ? "Strong" : m == Mode::Decrease ? "Sharply decreasing" : "Strongly disturbed";
    }
    return "";
}

Index evaluate(Series s, Mode mode, double binSeconds, double minHistoryHours) {
    Index out;
    s.erase(std::remove_if(s.begin(), s.end(), [](const std::pair<double, double>& p) {
        return !std::isfinite(p.first) || !std::isfinite(p.second);
    }), s.end());
    if (s.empty()) { out.reason = "no samples"; return out; }
    std::sort(s.begin(), s.end());
    dropOutliers(s);
    if (s.empty()) { out.reason = "no samples"; return out; }
    out.lastTime = s.back().first;

    // Bins on a fixed grid, so the same sample always lands in the same bin.
    std::map<long long, std::vector<std::pair<double, double>>> grid;
    for (const auto& p : s) grid[static_cast<long long>(std::floor(p.first / binSeconds))].push_back(p);
    for (const auto& [k, pts] : grid) {
        Bin b;
        b.t = (static_cast<double>(k) + 0.5) * binSeconds;
        b.samples = static_cast<int>(pts.size());
        double sum = 0.0;
        for (const auto& p : pts) sum += p.second;
        const double mean = sum / pts.size();
        if (mode == Mode::Variability) {
            // Minute-to-minute change: RMS of the differences between consecutive one-minute means,
            // over sqrt 2. On the 10-18 N4 data the second-to-second change is instrument noise (its
            // lag-1 autocorrelation is about -0.4, white noise gives -0.5), so the old per-sample
            // measure tracked the noise, not the field. One-minute means cut that noise about
            // eightfold and keep the minute-scale changes a disturbance brings; a standard deviation
            // would also count the slow daily swing, which is not a disturbance.
            std::map<long long, std::pair<double, int>> minutes;
            for (const auto& p : pts) {
                auto& m = minutes[static_cast<long long>(std::floor(p.first / 60.0))];
                m.first += p.second;
                ++m.second;
            }
            double ss = 0.0;
            int n = 0;
            long long prevKey = 0;
            double prevMean = 0.0;
            bool havePrev = false;
            for (const auto& [mk, m] : minutes) {
                if (m.second < 20) { havePrev = false; continue; }   // a minute mostly missing
                const double mm = m.first / m.second;
                if (havePrev && mk == prevKey + 1) { ss += (mm - prevMean) * (mm - prevMean); ++n; }
                prevKey = mk;
                prevMean = mm;
                havePrev = true;
            }
            if (n < 3) continue;
            b.value = std::sqrt(ss / (2.0 * n));
        } else {
            b.value = mean;
        }
        out.bins.push_back(b);
    }
    if (out.bins.empty()) { out.reason = "too few samples"; return out; }

    // The current level: the newest bin with a fair number of samples (a bin just started holds a
    // handful and would swing the index).
    std::size_t cur = out.bins.size() - 1;
    if (out.bins[cur].samples < 20 && cur > 0) --cur;
    out.current = out.bins[cur].value;
    out.currentSamples = out.bins[cur].samples;
    // A count channel with not one count in a full bin is switched off or the instrument has
    // changed mode (seen on N4 on 10-23: the proton channels at bytes 17-45 of ID 60 all read 0,
    // where on 10-18 the busiest one counted 35 a second). A real count of zero for ten minutes is
    // not plausible for these channels, so say "silent" rather than "Quiet".
    if (mode != Mode::Variability && out.current == 0.0 && out.currentSamples >= 20) {
        out.reason = "silent";
        return out;
    }

    std::vector<double> history;
    for (std::size_t i = 0; i < out.bins.size(); ++i)
        if (i != cur && out.bins[i].samples >= 10) history.push_back(out.bins[i].value);
    out.historyHours = (s.back().first - s.front().first) / 3600.0;
    if (history.size() < 4 || out.historyHours < minHistoryHours) {
        out.reason = "baseline still building";
        return out;
    }
    out.baseline = median(history);

    if (mode == Mode::Decrease) {
        if (out.baseline == 0.0) { out.reason = "zero baseline"; return out; }
        out.score = (out.current / out.baseline - 1.0) * 100.0;
        out.level = out.score > -3.0 ? Level::Quiet : out.score > -6.0 ? Level::Active
                  : out.score > -10.0 ? Level::Elevated : Level::Strong;
    } else {
        // A floor under the baseline keeps a channel that usually reads near zero from reporting
        // a huge ratio for a count of two.
        const double floor = mode == Mode::Ratio ? 1.0 : 1e-9;
        out.score = out.current / std::max(std::fabs(out.baseline), floor);
        const double a = mode == Mode::Ratio ? 1.5 : 2.0;
        const double b = mode == Mode::Ratio ? 3.0 : 4.0;
        const double c = mode == Mode::Ratio ? 10.0 : 8.0;
        out.level = out.score < a ? Level::Quiet : out.score < b ? Level::Active
                  : out.score < c ? Level::Elevated : Level::Strong;
    }
    return out;
}

}  // namespace nadirstation::spaceweather
