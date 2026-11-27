// Pass prediction for the Passes page: the same method as Orbitron -- SGP4/SDP4 from a TLE, the
// satellite seen from a fixed ground station, horizon crossings found by stepping and bisection.
//
// Plain C++ (std + libsgp4), no Qt, so it can run on worker threads and be checked on its own.
// Times are Unix seconds (UTC). Angles are degrees unless stated otherwise.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace nadirstation::pass {

struct Station {
    double latDeg = 0.0;      // north positive
    double lonDeg = 0.0;      // east positive
    double altM = 0.0;        // above the ellipsoid
};

struct TleEntry {
    std::string name;
    std::string line1;
    std::string line2;
    int norad = 0;
    std::string source;   // file it was read from (set by the caller), for display
};

// Reads 2-line or 3-line TLE text (Celestrak, Space-Track, Orbitron's .txt). Entries whose lines
// fail the checksum or do not parse are skipped; *skipped counts them.
std::vector<TleEntry> parseTle(const std::string& text, int* skipped = nullptr);

// What the station sees at one instant.
struct Look {
    bool ok = false;
    double azDeg = 0.0, elDeg = 0.0;
    double rangeKm = 0.0;
    double rangeRateKmS = 0.0;   // positive = moving away
    double latDeg = 0.0, lonDeg = 0.0, altKm = 0.0;   // sub-satellite point
    bool sunlit = true;          // satellite outside the Earth's (cylindrical) shadow
    double sunElDeg = 0.0;       // Sun elevation at the station
};

// One satellite, its TLE parsed once. Not thread-safe (libsgp4's Observer caches); use one per thread.
class Tracker {
public:
    explicit Tracker(const TleEntry& tle);
    ~Tracker();
    Tracker(const Tracker&) = delete;
    Tracker& operator=(const Tracker&) = delete;

    bool valid() const { return valid_; }
    const std::string& error() const { return error_; }
    const TleEntry& tle() const { return tle_; }
    double periodMinutes() const { return periodMin_; }
    double epochUnix() const { return epochUnix_; }

    // Position seen from `st`. `withSun` adds the sunlit / Sun elevation fields (slower).
    Look look(const Station& st, double unixTime, bool withSun = false) const;
    // Elevation only (the pass search calls this many times).
    double elevation(const Station& st, double unixTime) const;
    // Sub-satellite point; false if propagation fails (decayed).
    bool subPoint(double unixTime, double* latDeg, double* lonDeg, double* altKm) const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
    TleEntry tle_;
    bool valid_ = false;
    std::string error_;
    double periodMin_ = 0.0;
    double epochUnix_ = 0.0;
};

struct TrackPoint {
    double t = 0.0, azDeg = 0.0, elDeg = 0.0;
};

struct Pass {
    int sat = -1;                 // index into the satellite list given to predict()
    std::string name;
    int norad = 0;
    double aos = 0.0, tca = 0.0, los = 0.0;
    double aosAzDeg = 0.0, tcaAzDeg = 0.0, losAzDeg = 0.0;
    double maxElDeg = 0.0;
    bool startsBeforeWindow = false;   // already above the horizon at the window start
    bool endsAfterWindow = false;      // still above the horizon at the window end
    bool alwaysUp = false;             // never sets within the window (geostationary etc.)
    bool visible = false;              // sunlit while the station is in darkness (Sun < -6 deg): naked-eye
    std::vector<TrackPoint> track;     // for the sky plot
};

struct PredictOptions {
    double startUnix = 0.0;
    double days = 3.0;
    double horizonDeg = 0.0;      // AOS/LOS elevation
    double minPeakDeg = 0.0;      // passes peaking below this are dropped
    int trackPoints = 90;
};

// Passes of one satellite.
std::vector<Pass> predictOne(Tracker& tr, int satIndex, const Station& st, const PredictOptions& opt,
                             const std::atomic<bool>* cancel = nullptr);

// Passes of several satellites, spread over `threads` worker threads (0 = hardware concurrency),
// sorted by AOS. `done` (if given) counts finished satellites; `cancel` stops early.
std::vector<Pass> predictAll(const std::vector<TleEntry>& sats, const Station& st, const PredictOptions& opt,
                             std::atomic<int>* done = nullptr, const std::atomic<bool>* cancel = nullptr,
                             int threads = 0);

// Radius of the visibility circle on the ground (central angle, degrees) for a satellite at altKm,
// seen down to `horizonDeg` elevation.
double footprintRadiusDeg(double altKm, double horizonDeg = 0.0);

// Doppler shift (Hz) of a downlink at freqHz for a range rate in km/s.
inline double dopplerHz(double freqHz, double rangeRateKmS) { return -freqHz * rangeRateKmS / 299792.458; }

}  // namespace nadirstation::pass
