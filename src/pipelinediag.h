#pragma once
// What the Decoder page shows: one snapshot of the whole receive chain, stage by stage, built on the
// decoder's own thread (nadirstationdiag.cpp) about four times a second and queued to the GUI.
//
// Every stage carries a health, a headline value, a number for its sparkline, the counters the loss
// funnel needs, and a list of named metrics, each with its own health. Formatting is done where the
// numbers are known (the decoder), so the page only draws.
#include <QMetaType>
#include <QString>
#include <QVector>
#include <algorithm>
#include <cmath>

namespace nadirstation::diag {

enum Health : int { Idle = 0, Ok = 1, Warn = 2, Error = 3 };

struct Metric {
    QString name;
    QString value;
    int health = Idle;
    QString tip;        // optional: what the number means or what to check
};

struct Stage {
    QString key;        // stable id: iq, carrier, demod, sync, fec, data, product
    QString title;      // "Frame sync"
    QString detail;     // what this stage is on this link: "CCSDS ASM 1ACFFC1D"
    int health = Idle;
    QString headline;   // the big value on the node
    QString caption;    // what the headline is
    double spark = std::nan("");   // one value per snapshot for the sparkline (NaN = gap)
    QString sparkUnit;
    // Loss funnel: units that passed this stage and units it threw away, cumulative since the start.
    double passed = -1.0;
    double rejected = -1.0;
    QString unit;       // "frames", "CADUs", "lines"
    QString why;        // one sentence: why it has this colour, what to look at
    QVector<Metric> metrics;
};

struct Snapshot {
    QString link;       // "GK-2A LRIT"
    QString chain;      // "BPSK 128 ksym/s · r=1/2 Viterbi · RS(255,223) · CCSDS packets"
    qint64 ms = 0;      // wall clock, ms since epoch
    double streamSeconds = 0.0;   // signal time processed since the decoder started
    QVector<Stage> stages;
};

inline int worst(int a, int b) { return std::max(a, b); }

}  // namespace nadirstation::diag

Q_DECLARE_METATYPE(nadirstation::diag::Snapshot)
