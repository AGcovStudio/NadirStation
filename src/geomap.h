#pragma once
// Map products for the polar instruments: each sample located on the Earth from the TLE and its
// look angles, put on a latitude / longitude grid, and drawn with the coast and borders (Natural
// Earth, China point-of-view file, as the other map products of the program).
//
// Resampling is nearest-sample: every grid cell within a sample's footprint takes the value of the
// nearest sample (no averaging across samples, no smoothing). Cells that stay empty inside the swath
// -- with data on two opposite sides and at least four of eight neighbours -- take the median of
// their neighbours; measured cells are never changed.

#include <QImage>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nadirstation::pass { class Tracker; }

namespace nadirstation::polar {

// Ground point of a look from the satellite at time t: `crossDeg` off nadir across the track
// (positive to the right of the flight direction), `alongDeg` off nadir along it (positive forward).
class SwathGeo {
public:
    SwathGeo(const std::string& name, const std::string& line1, const std::string& line2);
    ~SwathGeo();
    bool valid() const;
    QString tleName() const;
    bool locate(double t, double crossDeg, double alongDeg, double* lat, double* lon) const;
    // Footprint half-size on the ground (km) for a look `crossDeg` off nadir with angular
    // spacing `stepDeg`, at time t (altitude from the TLE).
    double groundSpacingKm(double t, double crossDeg, double stepDeg) const;
private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

class GeoGrid {
public:
    explicit GeoGrid(int bands) : bands_(bands) {}
    // Samples: location, values (bands), footprint radius on the ground.
    void add(double lat, double lon, const float* values, float radiusKm);
    std::size_t samples() const { return lat_.size(); }
    // Grid at `resDeg`; returns false when there is nothing or the extent is out of range.
    bool build(double resDeg);
    int width() const { return W_; }
    int height() const { return H_; }
    double lon0() const { return lon0_; }
    double lat1() const { return lat1_; }
    double res() const { return res_; }
    // value of band b at cell (x, y); NaN when empty
    float at(int x, int y, int b) const { return cell_[(std::size_t(y) * std::size_t(W_) + std::size_t(x)) * std::size_t(bands_) + std::size_t(b)]; }
    int filledCells() const { return filled_; }

    struct Legend {
        QString title;            // first legend line
        QString detail;           // second legend line
        std::function<QRgb(double)> ramp;   // 0..1 -> colour (colour bar); null: no bar
        QString lowLabel, midLabel, highLabel;
        std::vector<std::pair<QRgb, QString>> classes;   // categorical legend instead of a bar
    };
    // Picture: cell colour from `colour` (return 0 for no data), `zoom` x `zoom` pixels per cell,
    // graticule, coast / borders from `mapsDir`, legend underneath.
    QImage render(const std::function<QRgb(const GeoGrid&, int, int)>& colour, int zoom, const QString& mapsDir,
                  const Legend& legend) const;
    // 16-bit grid of band b: value * scale + offset (0 = no data), and its world file (.pgw).
    bool saveRaw16(int band, double scale, double offset, const QString& pngPath) const;

private:
    int bands_;
    std::vector<float> lat_, lon_, val_, rad_;
    double refLon_ = 0.0;
    bool refSet_ = false;
    int W_ = 0, H_ = 0, filled_ = 0;
    double lon0_ = 0, lat1_ = 0, res_ = 0;
    std::vector<float> cell_;
};

// Colour ramps used by the map products (0..1).
QRgb rampMicrowave(double t);   // dark blue .. cyan .. green .. yellow .. red
QRgb rampCloudTop(double t);    // 0 warm (black) .. grey .. cold colours (cloud-top style)
QRgb rampDiverging(double t);   // blue .. white .. red

}  // namespace nadirstation::polar
