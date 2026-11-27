// Space-weather indices from ELEKTRO-L N4 GGAK records: each channel's recent level against the
// station's own history of it. Plain C++ so the arithmetic can be checked without the GUI.
//
// What these are and are not: the N4 channels arrive as instrument counts, and which instrument and
// channel each record field is was inferred from the record structure (ggak.h). No calibration to
// physical flux is applied. An index here says "this channel is N times its usual level", which is
// what an operator can act on, not "the flux is X".
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace nadirstation::spaceweather {

using Series = std::vector<std::pair<double, double>>;   // (unix time, value), any order

enum class Mode {
    Ratio,        // level against baseline: particles, X-ray / UV
    Decrease,     // drop below baseline in percent: cosmic rays (Forbush decrease)
    Variability,  // minute-to-minute change within each bin against the usual: magnetometer
};

enum class Level { Unknown = -1, Quiet = 0, Active = 1, Elevated = 2, Strong = 3 };

struct Bin { double t = 0.0; double value = 0.0; int samples = 0; };

struct Index {
    Level level = Level::Unknown;
    double current = 0.0;        // last bin's mean (Ratio, Decrease) or spread (Variability)
    double baseline = 0.0;       // median of the bins over the history
    double score = 0.0;          // ratio (Ratio, Variability) or percent change (Decrease)
    int currentSamples = 0;      // samples in the bin "current" comes from
    double lastTime = 0.0;       // newest sample
    double historyHours = 0.0;   // span the baseline was taken over
    std::vector<Bin> bins;       // for the plot
    std::string reason;          // why Unknown, when it is ("silent": a whole bin of zeros)
};

// binSeconds: 600 by default. minHistoryHours: below this the baseline is not trusted and the level
// is Unknown (the plot still shows). Outliers far from the median (other content in the same record
// slot) are dropped first.
Index evaluate(Series s, Mode mode, double binSeconds = 600.0, double minHistoryHours = 1.0);

// The worse of two levels (Unknown loses to anything known).
Level worse(Level a, Level b);

// Chinese wording, by mode: 平静/偏高/增强/强 (Ratio), 正常/下降/明显下降/大幅下降 (Decrease),
// 平静/扰动/较强扰动/强扰动 (Variability); 基线积累中 for Unknown.
const char* levelName(Level l, Mode m);
}  // namespace nadirstation::spaceweather
