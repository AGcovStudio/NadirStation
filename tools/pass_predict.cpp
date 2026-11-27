// Command-line pass prediction with the code of the Passes page (src/passpredict.*), for checking
// the results against Orbitron or another predictor.
//
//   NadirStation_PassPredict <tle file> <lat deg> <lon deg> [alt m] [days] [min peak deg] [NORAD ...]
//
// Times are printed in UTC.
#include "passpredict.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>

using namespace nadirstation::pass;

static std::string utc(double t) {
    const std::time_t s = std::time_t(t);
    char b[32] = "?";
    if (const std::tm* tm = std::gmtime(&s)) std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", tm);
    return b;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <tle file> <lat> <lon> [alt_m] [days] [min_peak_deg] [NORAD ...]\n", argv[0]);
        return 1;
    }
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    int skipped = 0;
    std::vector<TleEntry> all = parseTle(ss.str(), &skipped);
    Station st{std::atof(argv[2]), std::atof(argv[3]), argc > 4 ? std::atof(argv[4]) : 0.0};
    PredictOptions opt;
    opt.startUnix = double(std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch()).count());
    opt.days = argc > 5 ? std::atof(argv[5]) : 1.0;
    opt.minPeakDeg = argc > 6 ? std::atof(argv[6]) : 0.0;
    std::vector<TleEntry> sats;
    if (argc > 7) {
        for (int i = 7; i < argc; ++i)
            for (const TleEntry& e : all)
                if (e.norad == std::atoi(argv[i])) sats.push_back(e);
    } else {
        sats = all;
    }
    std::fprintf(stderr, "%zu satellites (%d unreadable), station %.5f %.5f %.0f m, %.1f days\n", sats.size(), skipped,
                 st.latDeg, st.lonDeg, st.altM, opt.days);
    std::atomic<int> done{0};
    const std::vector<Pass> passes = predictAll(sats, st, opt, &done);
    std::printf("%-24s %-19s %6s %-19s %6s %6s %-19s %6s %s\n", "satellite", "AOS (UTC)", "az", "TCA (UTC)", "maxEl",
                "az", "LOS (UTC)", "az", "vis");
    for (const Pass& p : passes)
        std::printf("%-24.24s %-19s %6.1f %-19s %6.1f %6.1f %-19s %6.1f %s%s\n", p.name.c_str(), utc(p.aos).c_str(),
                    p.aosAzDeg, utc(p.tca).c_str(), p.maxElDeg, p.tcaAzDeg, utc(p.los).c_str(), p.losAzDeg,
                    p.visible ? "visible" : "", p.alwaysUp ? " always-up" : "");
    return 0;
}
