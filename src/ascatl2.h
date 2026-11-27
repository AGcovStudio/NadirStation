#pragma once
// ASCAT on MetOp AHRPT: the six beams' echo, located on the Earth and put on a latitude/longitude
// grid ("L2" map products of the pass).
//
// Time of each pulse: the CCSDS secondary header of its packet (days since 2000-01-01, ms, us).
// A packet without a usable time (missing, or outside 2010..2040) takes the system clock at the
// moment it was received instead, and the products say so.
//
// Where each sample looks: the satellite's sub-point and track from the TLE (SGP4) at the pulse
// time; each beam is a line of 256 range samples at off-nadir angles from about 10 to 55 degrees,
// turned about the nadir by the beam's azimuth (fore / mid / aft, either side), after SatDump's
// projection settings for MetOp ASCAT. The ground point is found on a spherical Earth.
//
// What the values are: the echo power as broadcast, in dB, relative -- each beam and range sample is
// referred to its median over the pass, which takes out the incidence-angle fall-off and the beam
// gains. Calibrated sigma-0 and winds need the ground processor's calibration (antenna patterns,
// internal calibration and noise pulses), which the broadcast does not carry in usable form, so the
// products are relative backscatter maps, labelled as such.

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <array>
#include <string>
#include <vector>

namespace nadirstation::polar {

struct AscatPulse {
    int beam = 0;                 // 0..5 (APID 208..213)
    double time = 0.0;            // Unix seconds, UTC
    bool systemTime = false;      // time taken from the system clock (no usable packet time)
    std::array<float, 256> power{};
};

struct AscatL2Input {
    std::vector<AscatPulse> pulses;
    std::string tleName, tleLine1, tleLine2;   // empty: no products
    QString mapsDir;                           // holds ne_10m_admin_0_countries_chn.geojson (coast/borders)
};

// Writes the map products into `dir`; `log` gets one line on what was done or why not.
QStringList makeAscatL2(const AscatL2Input& in, const QString& dir, QString* log);

}  // namespace nadirstation::polar
