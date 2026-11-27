// Rendering core of the zoomable globe: an orthographic view of the Earth built from a world relief
// map and a geostationary full disk (GK-2A), sampled per screen pixel. Plain C++ so it can be
// checked without the GUI; the Qt widget (globeview.*) only feeds it and draws the overlays.
#pragma once

#include <cstdint>
#include <vector>

namespace nadirstation::globe {

// An image in memory, 8-bit RGB or 16-bit grey, row-major, no padding.
struct Rgb8 {
    int width = 0, height = 0;
    std::vector<std::uint8_t> data;   // width * height * 3
    bool empty() const { return width <= 0 || height <= 0 || data.empty(); }
};
struct Grey16 {
    int width = 0, height = 0;
    std::vector<std::uint16_t> data;
    bool empty() const { return width <= 0 || height <= 0 || data.empty(); }
};

// Where a geostationary full disk sits: the sub-satellite longitude, the disk centre in pixels and
// the scanning angle per pixel (radians), as measured from the limb (gk2a::measureDisk).
struct DiskGeometry {
    double subLonDeg = 128.2;
    double centre = 1100.0;
    double radPerPixel = 0.0;
    bool valid() const { return radPerPixel > 0.0; }
};

enum class Layer {
    TrueColour,   // relief map, clouds from the brightness temperature, night side darkened
    Product,      // the satellite product image itself (rainbow, enhanced...) over a dimmed map
    EnhancedTops, // true colour with clouds, cold cloud tops (< 240 K) coloured by temperature
};
// Colour of the enhanced cloud-top scale for a brightness temperature (K); false above 240 K.
bool enhancedTopColour(double kelvin, double* r, double* g, double* b);

struct View {
    double centreLatDeg = 20.0;
    double centreLonDeg = 128.2;
    double zoom = 1.0;              // 1 = whole globe fits the shorter side
    int width = 800, height = 600;
};

// A latitude/longitude (plate carrée) image, as the regional products are written: the centre of
// the upper-left pixel and the pixel size in degrees (the .pgw world file / GK2A_Region.json).
struct GeoBox {
    double lon0 = 0.0, lat0 = 0.0;      // centre of the upper-left pixel
    double dLon = 0.0, dLat = 0.0;      // pixel size, both positive (rows go south)
    bool valid() const { return dLon > 0.0 && dLat > 0.0; }
};

struct Scene {
    const Rgb8* relief = nullptr;       // equirectangular, -180..180, 90..-90
    const Grey16* tbb = nullptr;        // disk, 0.01 K, 0 = no data
    const Rgb8* product = nullptr;      // disk product of the same geometry, or a lat/lon image (geo)
    GeoBox geo;                         // valid: `product` is a lat/lon image, not a disk
    DiskGeometry disk;
    Layer layer = Layer::TrueColour;
    bool night = true;                  // darken where the sun is down
    double sunLatDeg = 0.0, sunLonDeg = 0.0;
    bool sunKnown = false;
};

// Screen pixel -> latitude/longitude. False off the globe.
bool screenToLatLon(const View& v, double sx, double sy, double* latDeg, double* lonDeg);
// Latitude/longitude -> screen pixel. False on the far side.
bool latLonToScreen(const View& v, double latDeg, double lonDeg, double* sx, double* sy);
// Radius of the globe on screen, in pixels.
double globeRadius(const View& v);

// Earth point -> disk pixel (column, row). False when the satellite cannot see it; *edge is the
// cosine of the satellite zenith angle, used to fade the disk out towards its limb.
bool diskPixel(const DiskGeometry& g, double latDeg, double lonDeg, double* col, double* row, double* edge);

// Brightness temperature (K) at a point, NaN where there is none.
double tbbAt(const Scene& s, double latDeg, double lonDeg);
// Mean brightness temperature (K) of a raster of the same disk over (2*radius+1)^2 pixels around a
// point, ignoring pixels without data; NaN when the satellite cannot see the point.
double sampleTbb(const Grey16& g, const DiskGeometry& d, double latDeg, double lonDeg, int radius = 1);

// Renders rows [y0, y1) of the view into rgba (width * height * 4, premultiplied opaque).
void renderRows(const Scene& s, const View& v, int y0, int y1, int step, std::uint8_t* rgba);

// Sub-solar point for a UTC time (seconds since 1970).
void subSolarPoint(double unixSeconds, double* latDeg, double* lonDeg);

}  // namespace nadirstation::globe
