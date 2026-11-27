#include "imageproducts.h"
#include "orbit.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QColor>
#include <QRegularExpression>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

namespace {
std::array<uchar, 7> glyph5x7(char c) {
    switch (c) {
    case 'A': return {14,17,17,31,17,17,17}; case 'B': return {30,17,17,30,17,17,30};
    case 'C': return {14,17,16,16,16,17,14}; case 'D': return {30,17,17,17,17,17,30};
    case 'E': return {31,16,16,30,16,16,31}; case 'F': return {31,16,16,30,16,16,16};
    case 'G': return {14,17,16,23,17,17,15}; case 'H': return {17,17,17,31,17,17,17};
    case 'I': return {14,4,4,4,4,4,14}; case 'L': return {16,16,16,16,16,16,31};
    case 'M': return {17,27,21,21,17,17,17}; case 'N': return {17,25,21,19,17,17,17};
    case 'O': return {14,17,17,17,17,17,14}; case 'P': return {30,17,17,30,16,16,16};
    case 'R': return {30,17,17,30,20,18,17}; case 'S': return {15,16,16,14,1,1,30};
    case 'T': return {31,4,4,4,4,4,4}; case 'U': return {17,17,17,17,17,17,14};
    case 'V': return {17,17,17,17,17,10,4}; case 'W': return {17,17,17,21,21,21,10};
    case 'X': return {17,17,10,4,10,17,17}; case 'Y': return {17,17,10,4,4,4,4};
    case 'K': return {17,18,20,24,20,18,17};
    case '0': return {14,17,19,21,25,17,14}; case '1': return {4,12,4,4,4,4,14};
    case '2': return {14,17,1,2,4,8,31}; case '3': return {30,1,1,14,1,1,30};
    case '4': return {2,6,10,18,31,2,2}; case '5': return {31,16,16,30,1,1,30};
    case '6': return {14,16,16,30,17,17,14}; case '7': return {31,1,2,4,8,8,8};
    case '8': return {14,17,17,14,17,17,14}; case '9': return {14,17,17,15,1,1,14};
    case '-': return {0,0,0,31,0,0,0}; case '/': return {1,2,2,4,8,8,16};
    case '%': return {17,2,4,8,17,0,0}; case '=': return {0,31,0,31,0,0,0};
    case ':': return {0,4,4,0,4,4,0}; case '.': return {0,0,0,0,0,12,12};
    default: return {0,0,0,0,0,0,0};
    }
}

void drawAscii(QImage& image, int x, int y, const QString& text, QRgb color, int scale = 2) {
    for (const QChar q : text.toUpper()) {
        const auto glyph = glyph5x7(q.toLatin1());
        for (int gy = 0; gy < 7; ++gy) for (int gx = 0; gx < 5; ++gx) {
            if (!(glyph[gy] & (1u << (4 - gx)))) continue;
            for (int sy = 0; sy < scale; ++sy) for (int sx = 0; sx < scale; ++sx) {
                const int px = x + gx * scale + sx, py = y + gy * scale + sy;
                if (px >= 0 && py >= 0 && px < image.width() && py < image.height())
                    reinterpret_cast<QRgb*>(image.scanLine(py))[px] = color;
            }
        }
        x += 6 * scale;
    }
}

void drawSolidRect(QImage& image, int x, int y, int width, int height, QRgb color) {
    for (int py = qMax(0, y); py < qMin(image.height(), y + height); ++py) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(py));
        std::fill(row + qMax(0, x), row + qMin(image.width(), x + width), color);
    }
}

void collectGeoJsonRings(const QJsonValue& value, const QRectF& geoBounds,
                         const QSize& imageSize, QPainterPath& path) {
    if (!value.isArray()) return;
    const QJsonArray array = value.toArray();
    if (array.size() >= 4 && array.first().isArray() && array.first().toArray().size() >= 2 &&
        array.first().toArray().first().isDouble()) {
        QPolygonF polygon;
        polygon.reserve(array.size());
        for (const QJsonValue& pointValue : array) {
            const QJsonArray point = pointValue.toArray();
            if (point.size() < 2) continue;
            const double longitude = point[0].toDouble();
            const double latitude = point[1].toDouble();
            const double x = (longitude - geoBounds.left()) / geoBounds.width() * imageSize.width();
            const double y = (geoBounds.top() - latitude) / geoBounds.height() * imageSize.height();
            polygon.append(QPointF(x, y));
        }
        if (polygon.size() >= 3) path.addPolygon(polygon);
        return;
    }
    for (const QJsonValue& child : array)
        collectGeoJsonRings(child, geoBounds, imageSize, path);
}

QImage landMaskForRegion(double centerLat, double centerLon, const QSize& size) {
    if (!std::isfinite(centerLat) || !std::isfinite(centerLon) || size.isEmpty()) return {};
    const QRectF geoBounds(centerLon - 34.0, centerLat + 21.0, 68.0, 42.0);
    QFile file(QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("resources/maps/ne_110m_admin_0_countries.geojson")));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    QImage land(size, QImage::Format_Grayscale8);
    land.fill(0);
    QPainter painter(&land);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    for (const QJsonValue& featureValue : root.value(QStringLiteral("features")).toArray()) {
        const QJsonObject geometry = featureValue.toObject().value(QStringLiteral("geometry")).toObject();
        QPainterPath country;
        country.setFillRule(Qt::OddEvenFill);
        collectGeoJsonRings(geometry.value(QStringLiteral("coordinates")), geoBounds, size, country);
        painter.drawPath(country);
    }
    painter.end();
    return land;
}

// Provenance of the geometry used for this run, written into every sidecar (set in main()).
QString g_navigationStatus;

QDateTime observationMidpoint(const QString& directory, const QJsonObject& manifest) {
    QDateTime end = QDateTime::fromString(manifest.value("created_utc").toString(), Qt::ISODateWithMs);
    if (!end.isValid()) end = QDateTime::fromString(manifest.value("created_utc").toString(), Qt::ISODate);
    const QString folder = QFileInfo(directory).fileName();
    const auto match = QRegularExpression(QStringLiteral("^(\\d{4})(\\d{2})(\\d{2})_(\\d{2})(\\d{2})(\\d{2})_(\\d{3})$")).match(folder);
    if (!match.hasMatch()) return end;
    const QString startString = QStringLiteral("%1-%2-%3T%4:%5:%6.%7Z")
        .arg(match.captured(1), match.captured(2), match.captured(3), match.captured(4),
             match.captured(5), match.captured(6), match.captured(7));
    const QDateTime start = QDateTime::fromString(startString, Qt::ISODateWithMs);
    if (!start.isValid()) return end;
    return end.isValid() ? start.addMSecs(start.msecsTo(end) / 2) : start;
}

bool saveProduct(const QDir& directory, const QString& name, const QImage& image,
                 const QString& label, const QString& units, const QString& method,
                 const QString& satellite, const QString& norad,
                 const nadirstation::orbit::SatellitePosition& raw,
                 const nadirstation::orbit::SatellitePosition& camera,
                 const QDateTime& epoch, QJsonArray& manifestProducts,
                 int projectedOffsetX = 0, int projectedOffsetY = 0,
                 double gcpLatitude = 0.0, double gcpLongitude = 0.0,
                 int gcpPixelX = 0, int gcpPixelY = 0,
                 const QJsonObject& details = {}, bool registerInManifest = true) {
    if (image.isNull()) return false;
    const QString path = directory.filePath(name);
    const bool hasFooter = details.value(QStringLiteral("display_layout")).toString()
        .contains(QStringLiteral("appended footer"));
    const int inferredFooterHeight = name.contains(QStringLiteral("_CFR_")) ? 78 : 74;
    const int defaultMapHeight = hasFooter ? image.height() - inferredFooterHeight : image.height();
    const int mapHeight = std::clamp(details.value(QStringLiteral("map_height_pixels")).toInt(defaultMapHeight),
                                     1, image.height());
    QImage map = image.copy(0, 0, image.width(), mapHeight);
    int repairedRows = 0;
    const bool cloudFraction = label.contains(QStringLiteral("CFR"), Qt::CaseInsensitive) ||
        name.contains(QStringLiteral("CFR"), Qt::CaseInsensitive);
    const bool calibratedTemperature = name.contains(QStringLiteral("TBB"), Qt::CaseInsensitive) ||
        name.contains(QStringLiteral("CTT"), Qt::CaseInsensitive);
    const bool preserveNightSideDisplay = details.value(QStringLiteral("night_side_mask_applied")).toBool() &&
        name.contains(QStringLiteral("Region_VIS_Reflectance_Enhanced"));
    const bool preserveNumericSamples = details.value(QStringLiteral("preserve_numeric_samples")).toBool();
    if (!preserveNightSideDisplay && !preserveNumericSamples) {
        if (map.format() == QImage::Format_Indexed8 || cloudFraction ||
            (map.format() == QImage::Format_Grayscale16 && calibratedTemperature))
            map = nadirstation::products::repairMissingLinesMedian(map, 2, &repairedRows);
        else if (map.format() != QImage::Format_Grayscale16)
            map = nadirstation::products::repairMissingLines(map, 8, &repairedRows);
    }
    int filteredPixels = 0;
    // NADIRSTATION_NO_MEDIAN_BLUR=1 writes the products without the 3x3 stage, so the filter's cost
    // can be measured by diffing two otherwise identical runs instead of inferred. The stage
    // is a MODE vote for indexed rasters (see medianBlurPreservingFormat), which on a binary
    // cloud mask is majority rule and erases structure smaller than half the window. Whether
    // that measurably harms the products is a question for a controlled comparison, not for
    // reading the code, so the switch exists to make that comparison possible.
    const bool applyMedianBlur =
        qEnvironmentVariableIntValue("NADIRSTATION_NO_MEDIAN_BLUR") == 0;
    if (!preserveNightSideDisplay && !preserveNumericSamples && applyMedianBlur)
        map = nadirstation::products::medianBlurPreservingFormat(map, 3, &filteredPixels, cloudFraction);
    QImage output = image.copy();
    if (mapHeight == image.height()) {
        output = map;
    } else {
        QPainter painter(&output);
        painter.drawImage(0, 0, map);
    }
    // Hard guarantee: a file whose name does not mark it as a colour display is a
    // measurement raster and must be written as 16-bit grayscale. Several call
    // paths reach this point with an RGB buffer (their legend footer was appended
    // upstream), which silently destroyed the physical values of the numeric
    // raster; refusing to write RGB here makes that class of bug impossible.
    //
    // NOT YET FIXED HERE: indexed regional products lose their class codes on re-encoding.
    //
    // The cloud mask reaches this function already widened to RGB32 (visible
    // below as qt_format_at_entry), so its index codes 0/1/255 have been turned
    // into colour-table greys before this point. A first attempt to preserve the
    // indices here did nothing, because the "is Indexed8" test never held.
    //
    // The fix belongs where the mask is produced (indexedCloudMask) or where it
    // is consumed -- and it must cover the consumers, because
    // regional_product_preview.cpp reads mask[x] != 255, mask[x] == 0 and
    // mask[x] == 1 as indices, and cloudFractionFromMask uses row[x] > 1 as its
    // validity test. Changing only the writer would leave those three tests
    // reading colour greys.
    if (!name.contains(QStringLiteral("Enhanced")) &&
        output.format() != QImage::Format_Grayscale16 &&
        output.format() != QImage::Format_Grayscale8 &&
        output.format() != QImage::Format_Indexed8) {
        // A GREYSCALE raster that has already been widened to RGB32 must not be pushed
        // through convertToFormat(Grayscale16).
        //
        // convertToFormat widens 8 bits to 16 by multiplying, so a cloud-fraction raster
        // holding 0..100 arrives in the file as 0..25700 while its own sidecar declares
        // "grayscale value 0-100". Measured on scene 20261005_060012_773 before this fix:
        // the file held the five distinct values 0, 6460, 12842, 19272, 25701 -- every one
        // of them equal to the true percentage shifted left by 8, so the real product was
        // 0, 25, 50, 75, 100 and the file said 25.3 %, 50.4 %, 75.6 %, 100.8 %.
        //
        // The grey case is recognised from the pixels rather than the format, because by
        // this point the format no longer distinguishes it: addProductLegend() paints the
        // footer canvas as RGB32 even for a grey product. R == G == B over the RASTER AREA
        // means the buffer is a grey image in a colour container and its byte values are the
        // measurement, so they are carried into the 16-bit channel unchanged instead of
        // being scaled by convertToFormat's 8-to-16 widening.
        //
        // The test covers only the raster rows, not the appended legend footer. The footer
        // is drawn with coloured text (ink qRgb(235,240,246)), so scanning the whole image
        // finds R != G != B there and rejects a raster that is grey everywhere it matters --
        // which is exactly what happened on the first attempt at this fix.
        const int rasterHeight = details.value(QStringLiteral("map_height_pixels"))
                                     .toInt(output.height());
        bool greyInColourContainer = false;
        if (output.format() == QImage::Format_RGB32) {
            greyInColourContainer = true;
            const int rows = qBound(1, rasterHeight, output.height());
            for (int y = 0; y < rows && greyInColourContainer; ++y) {
                const auto* row = reinterpret_cast<const QRgb*>(output.constScanLine(y));
                for (int x = 0; x < output.width(); ++x) {
                    if (qRed(row[x]) != qGreen(row[x]) || qGreen(row[x]) != qBlue(row[x])) {
                        greyInColourContainer = false;
                        break;
                    }
                }
            }
        }
        if (greyInColourContainer) {
            QImage widened(output.size(), QImage::Format_Grayscale16);
            for (int y = 0; y < output.height(); ++y) {
                const auto* src = reinterpret_cast<const QRgb*>(output.constScanLine(y));
                auto* dst = reinterpret_cast<quint16*>(widened.scanLine(y));
                for (int x = 0; x < output.width(); ++x)
                    dst[x] = static_cast<quint16>(qRed(src[x]));
            }
            output = widened;
        } else {
            output = output.convertToFormat(QImage::Format_Grayscale16);
        }
        if (output.height() > 0 && details.contains(QStringLiteral("map_height_pixels"))) {
            const int mapRows = details.value(QStringLiteral("map_height_pixels")).toInt(output.height());
            if (mapRows > 0 && mapRows < output.height()) output = output.copy(0, 0, output.width(), mapRows);
        }
    }

    // Record the formats the writer actually received. The first attempt to
    // preserve index codes silently did nothing, because by then the raster was
    // no longer indexed; guessing which upstream step widened it was wasted
    // effort, so the formats are written into the sidecar and read back instead.
    // Qt format codes: 3=Indexed8, 4=RGB32, 24=Grayscale8, 28=Grayscale16.
    const int formatAtEntry = int(image.format());
    const int formatAfterMap = int(map.format());
    const int formatAtWrite = int(output.format());
    if (!output.save(path, "PNG")) return false;
    QJsonObject meta;
    meta["format"] = QStringLiteral("FY2-Regional-Product-1");
    meta["product"] = label;
    meta["source_observation"] = directory.dirName();
    meta["units"] = units;
    meta["method_and_limitations"] = method;
    meta["missing_line_repair_rows"] = repairedRows;
    meta["median_blur_kernel"] = QStringLiteral("3x3");
    meta["median_blur_changed_pixels"] = filteredPixels;
    meta["qt_format_at_entry"] = formatAtEntry;      // 3=Indexed8 4=RGB32 24=Gray8 28=Gray16
    meta["qt_format_after_map"] = formatAfterMap;
    meta["qt_format_written"] = formatAtWrite;
    meta["map_height_pixels"] = mapHeight;
    meta["satellite"] = satellite;
    meta["norad_id"] = norad;
    meta["tle_epoch_utc"] = raw.tleEpochUtc;
    meta["tle_propagated_at_utc"] = epoch.toUTC().toString(Qt::ISODateWithMs);
    meta["tle_subpoint_longitude_deg"] = raw.subpointLongitudeDeg;
    meta["tle_subpoint_latitude_deg"] = raw.subpointLatitudeDeg;
    meta["camera_scan_longitude_deg"] = camera.subpointLongitudeDeg;
    meta["camera_scan_latitude_deg"] = camera.subpointLatitudeDeg;
    if (projectedOffsetX != 0 || projectedOffsetY != 0) {
        meta["navigation_status"] = QStringLiteral("Approximate one-point GCP translation applied for this observation; accuracy away from the control point is unverified.");
        meta["gcp_latitude_deg"] = gcpLatitude;
        meta["gcp_longitude_deg"] = gcpLongitude;
        meta["gcp_observed_source_x"] = gcpPixelX;
        meta["gcp_observed_source_y"] = gcpPixelY;
        meta["projected_translation_x_pixels"] = projectedOffsetX;
        meta["projected_translation_y_pixels"] = projectedOffsetY;
    } else {
        meta["navigation_status"] = g_navigationStatus.isEmpty()
            ? QStringLiteral("TLE longitude drift applied; camera assumed Earth-pointing/equatorial. GCP validation still needed for absolute alignment.")
            : g_navigationStatus;
    }
    for (auto it = details.constBegin(); it != details.constEnd(); ++it)
        meta.insert(it.key(), it.value());

    // Every projected numeric PNG must carry a standard world file.  The
    // ordinary regional grid is WGS-84 longitude/latitude (68 x 42 degrees),
    // while published-method CSR rasters provide their coarser pixel-centre
    // spacing and origin explicitly in the sidecar.  Never georeference a
    // legend-footer image as if the footer were part of the map.
    QFile::remove(path + QStringLiteral(".pgw"));
    QFile::remove(path + QStringLiteral(".prj"));
    const auto number = [](double value) { return QString::number(value, 'g', 16); };
    const double gridLon = meta.value(QStringLiteral("projection_center_longitude_deg")).toDouble(
        std::numeric_limits<double>::quiet_NaN());
    const double gridLat = meta.value(QStringLiteral("projection_center_latitude_deg")).toDouble(
        std::numeric_limits<double>::quiet_NaN());
    double pixelLon = 68.0 / output.width();
    double pixelLat = 42.0 / qMax(1, mapHeight);
    double upperLeftCenterLon = gridLon - 34.0 + pixelLon * 0.5;
    double upperLeftCenterLat = gridLat + 21.0 - pixelLat * 0.5;
    const QJsonArray coarseSpacing = meta.value(QStringLiteral("aggregated_grid_degrees_per_pixel")).toArray();
    if (coarseSpacing.size() == 2 &&
        meta.contains(QStringLiteral("aggregated_grid_origin_lon_deg")) &&
        meta.contains(QStringLiteral("aggregated_grid_origin_lat_deg"))) {
        pixelLon = coarseSpacing[0].toDouble(pixelLon);
        pixelLat = coarseSpacing[1].toDouble(pixelLat);
        upperLeftCenterLon = meta.value(QStringLiteral("aggregated_grid_origin_lon_deg")).toDouble();
        upperLeftCenterLat = meta.value(QStringLiteral("aggregated_grid_origin_lat_deg")).toDouble();
    }
    const bool hasGeoReference = std::isfinite(gridLon) && std::isfinite(gridLat) &&
        output.width() > 0 && mapHeight > 0 && output.height() == mapHeight &&
        std::isfinite(pixelLon) && std::isfinite(pixelLat) && pixelLon > 0.0 && pixelLat > 0.0;
    meta["crs"] = QStringLiteral("EPSG:4326");
    meta["grid_registration"] = QStringLiteral("pixel-centre");
    bool worldFileWritten = false;
    bool projectionFileWritten = false;
    if (hasGeoReference) {
        meta["pixel_size_degrees"] = QJsonArray{pixelLon, -pixelLat};
        meta["upper_left_pixel_center_lonlat_deg"] = QJsonArray{upperLeftCenterLon, upperLeftCenterLat};
        QFile worldFile(path + QStringLiteral(".pgw"));
        if (worldFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            const QByteArray world = (number(pixelLon) + QLatin1Char('\n') +
                QStringLiteral("0\n0\n") + number(-pixelLat) + QLatin1Char('\n') +
                number(upperLeftCenterLon) + QLatin1Char('\n') +
                number(upperLeftCenterLat) + QLatin1Char('\n')).toUtf8();
            const bool writeSucceeded = worldFile.write(world) == world.size();
            worldFile.close();
            worldFileWritten = writeSucceeded && worldFile.error() == QFileDevice::NoError;
        }
        QFile projectionFile(path + QStringLiteral(".prj"));
        if (projectionFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            const QByteArray projection = "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433],AUTHORITY[\"EPSG\",\"4326\"]]";
            projectionFileWritten = projectionFile.write(projection) == projection.size();
            projectionFile.close();
        }
    }
    meta["world_file_written"] = worldFileWritten;
    meta["projection_file_written"] = projectionFileWritten;
    QFile sidecar(path + QStringLiteral(".json"));
    if (sidecar.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        sidecar.write(QJsonDocument(meta).toJson(QJsonDocument::Indented));
        sidecar.close();
    }
    QJsonObject item;
    item["path"] = name;
    item["absolute_path"] = QFileInfo(path).absoluteFilePath();
    // Regenerating the same observation must replace its manifest record,
    // not append a duplicate entry for every product refresh.
    QJsonArray uniqueProducts;
    for (const QJsonValue& existing : manifestProducts) {
        if (existing.toObject().value("path").toString() != name)
            uniqueProducts.append(existing);
    }
    manifestProducts = uniqueProducts;
    // Measurement rasters are written next to the display products but stay out
    // of the Image viewer list: the viewer is for pictures, and a greyscale
    // numeric raster next to its own colour rendering is just noise in the
    // product selector. Their sidecar JSON documents them either way.
    if (registerInManifest) manifestProducts.append(item);
    return true;
}

// The Image viewer lists pictures, not measurements. A regional product belongs
// in it when it carries a colour rendering (the "*_Enhanced" colour companion of
// a numeric raster) or when the numeric raster is itself a colour-coded
// categorical map (cloud mask, cloud class, cloud fraction). Plain greyscale
// numeric rasters and the imported official references stay on disk with their
// sidecars but out of the product list.
bool isViewerProduct(const QString& name) {
    if (name.contains(QStringLiteral("Enhanced"))) return true;
    return name.contains(QStringLiteral("_CLD_")) || name.contains(QStringLiteral("_CLC_")) ||
           name.contains(QStringLiteral("_CFR_"));
}

QColor interpolateColor(const QColor& a, const QColor& b, double t) {
    return QColor(qRound(a.red() + (b.red() - a.red()) * t),
                  qRound(a.green() + (b.green() - a.green()) * t),
                  qRound(a.blue() + (b.blue() - a.blue()) * t));
}

// One colour ramp per product family, shared by the map rendering and by the
// appended legend. Keeping a single source of truth is what stops a legend from
// showing a colour that the map never uses, which is what happened to the LST
// product: the map was written as a plain greyscale raster while its legend drew
// the standard five-stop colour ramp.
struct ColorRamp {
    double minimum;
    double maximum;
    QVector<QColor> stops;
};

ColorRamp colorRampFor(const QString& fileName) {
    if (fileName.contains(QStringLiteral("_FOG_")))
        return {0.0, 100.0, {QColor(25, 45, 80), QColor(255, 70, 170)}};
    if (fileName.contains(QStringLiteral("_CTH_")))
        return {0.0, 20000.0, {QColor(35, 70, 170), QColor(30, 170, 210), QColor(65, 190, 110),
                               QColor(245, 210, 55), QColor(225, 75, 45)}};
    if (fileName.contains(QStringLiteral("_LST_")))
        return {260.0, 340.0, {QColor(35, 70, 170), QColor(30, 170, 210), QColor(65, 190, 110),
                               QColor(245, 210, 55), QColor(225, 75, 45)}};
    if (fileName.contains(QStringLiteral("_OLR_")))
        return {120.0, 340.0, {QColor(35, 70, 170), QColor(30, 170, 210), QColor(65, 190, 110),
                               QColor(245, 210, 55), QColor(225, 75, 45)}};
    if (fileName.contains(QStringLiteral("_UTH_")))
        return {0.0, 100.0, {QColor(40, 70, 170), QColor(35, 175, 220), QColor(90, 210, 150),
                             QColor(240, 220, 90), QColor(220, 110, 70)}};
    if (fileName.contains(QStringLiteral("_PRE_")))
        return {0.0, 0.30, {QColor(35, 70, 170), QColor(30, 170, 210), QColor(65, 190, 110),
                            QColor(245, 210, 55), QColor(225, 75, 45)}};
    if (fileName.contains(QStringLiteral("_SST_MCSST")))
        return {285.0, 305.0, {QColor(35, 70, 170), QColor(30, 170, 210), QColor(65, 190, 110),
                               QColor(245, 210, 55), QColor(225, 75, 45)}};
    if (fileName.contains(QStringLiteral("_SST_")))
        return {270.0, 310.0, {QColor(35, 70, 170), QColor(30, 170, 210), QColor(65, 190, 110),
                               QColor(245, 210, 55), QColor(225, 75, 45)}};
    if (fileName.contains(QStringLiteral("_CTT_")))
        return {180.0, 320.0, {QColor(45, 80, 220), QColor(45, 205, 235), QColor(55, 190, 105),
                               QColor(245, 220, 55), QColor(245, 145, 45), QColor(220, 50, 55)}};
    return {0.0, 0.0, {}};
}

// colour = f(value) using exactly the ramp the legend will draw.
QColor rampColor(const ColorRamp& ramp, double value) {
    if (ramp.stops.size() < 2 || ramp.maximum <= ramp.minimum) return QColor(115, 125, 138);
    const double t = qBound(0.0, (value - ramp.minimum) / (ramp.maximum - ramp.minimum), 1.0) *
        (ramp.stops.size() - 1);
    const int i = qMin(static_cast<int>(ramp.stops.size()) - 2, static_cast<int>(t));
    return interpolateColor(ramp.stops[i], ramp.stops[i + 1], t - i);
}

QImage addProductLegend(const QImage& map, const QString& fileName) {
    if (map.isNull()) return {};
    // An indexed raster is returned untouched.
    //
    // Every branch below paints the map onto an RGB32 canvas to make room for a
    // footer, and QImage::convertToFormat(Indexed8 -> RGB32) applies the colour
    // table. For a categorical product that is fatal: the codes ARE the data, and
    // 255 is an "invalid" sentinel rather than a brightness. Measured before this
    // guard: the cloud mask's 0/1/255 reached the file as 9546/31861/61678, so the
    // cloud code was unrecoverable and the product could not be used at all.
    // The legend is a display convenience; the product is a measurement raster,
    // so where they conflict the measurement wins.
    if (map.format() == QImage::Format_Indexed8 &&
        !fileName.contains(QStringLiteral("Enhanced"))) return map;
    // The same holds for continuous measurement rasters. Without this guard the LST, CTH and FOG
    // branches below also matched the numeric file names, painted the 16-bit values onto an RGB
    // canvas and the saved "numeric" raster held display greys instead of values (LST: 255 in
    // every pixel, i.e. 2.55 K; CTH and FOG equally destroyed). Found 2026-10-17 from the
    // "fmt before legend" trace (28 -> 4, 840 -> 918 rows).
    if ((map.format() == QImage::Format_Grayscale16 || map.format() == QImage::Format_Grayscale8) &&
        !fileName.contains(QStringLiteral("Enhanced"))) return map;
    QStringList labels;
    QVector<QColor> colors;
    // Any family with a shared ramp gets its legend drawn from that ramp, so the
    // footer strip and the map pixels can never disagree. Only display images get
    // a footer: a numeric measurement raster must stay a plain raster.
    const ColorRamp sharedRamp = fileName.contains(QStringLiteral("Enhanced"))
        ? colorRampFor(fileName) : ColorRamp{0.0, 0.0, {}};
    if (sharedRamp.stops.size() >= 2) {
        QImage canvas(map.width(), map.height() + 78, QImage::Format_RGB32);
        canvas.fill(QColor(12, 18, 25));
        const QImage rgbMap = map.convertToFormat(QImage::Format_RGB32);
        for (int y = 0; y < map.height(); ++y)
            std::memcpy(canvas.scanLine(y), rgbMap.constScanLine(y),
                        static_cast<std::size_t>(map.width()) * 4);
        const QRgb ink = qRgb(235, 240, 246);
        QString caption;
        QString unitSuffix;
        if (fileName.contains(QStringLiteral("_FOG_"))) {
            caption = QStringLiteral("FOG / LOW STRATUS CANDIDATE CONFIDENCE - SPLIT WINDOW + WARM CLOUD TOP");
            unitSuffix = QStringLiteral("%");
        } else if (fileName.contains(QStringLiteral("_CTH_"))) {
            caption = QStringLiteral("CTH IR1 CLOUD-TOP HEIGHT CANDIDATE - STANDARD ATMOSPHERE INVERSION; LOW=WARM TOP");
            unitSuffix = QStringLiteral("km");
        } else if (fileName.contains(QStringLiteral("_LST_"))) {
            caption = QStringLiteral("LST CLEAR-SKY SPLIT-WINDOW CANDIDATE - K; NO LAND MASK OR EMISSIVITY CORRECTION");
            unitSuffix = QStringLiteral("K");
        } else if (fileName.contains(QStringLiteral("_OLR_"))) {
            caption = fileName.contains(QStringLiteral("StefanBoltzmann"))
                ? QStringLiteral("OLR PROXY W/M2 - SIGMA T^4 FROM IR1 TBB; NOT BROADBAND FLUX")
                : QStringLiteral("OLR NARROWBAND-TO-BROADBAND ESTIMATE; W/M2");
            unitSuffix = QStringLiteral("W/m2");
        } else if (fileName.contains(QStringLiteral("_SST_"))) {
            caption = fileName.contains(QStringLiteral("MCSST"))
                ? QStringLiteral("SST MCSST (BUOY-VALIDATED, FY-2G); K; GRAY = LAND, CLOUD OR NOT RETRIEVED")
                : QStringLiteral("SST CLEAR-OCEAN SPLIT-WINDOW CANDIDATE; K");
            unitSuffix = QStringLiteral("K");
        } else if (fileName.contains(QStringLiteral("_UTH_"))) {
            caption = QStringLiteral("UTH CALIBRATED ESTIMATE; RELATIVE HUMIDITY PERCENT");
            unitSuffix = QStringLiteral("%");
        } else if (fileName.contains(QStringLiteral("_PRE_"))) {
            caption = QStringLiteral("PRE 1-HOUR CALIBRATED ESTIMATE; MM");
            unitSuffix = QStringLiteral("mm");
        } else if (fileName.contains(QStringLiteral("_CTT_"))) {
            caption = QStringLiteral("CTT IR1 CLOUD-TOP TEMPERATURE CANDIDATE - GRAY=NO CLOUD; NOT CLOUD HEIGHT");
            unitSuffix = QStringLiteral("K");
        }
        drawAscii(canvas, 18, map.height() + 7, caption, ink);
        const int left = 20, top = map.height() + 39, width = qMin(460, map.width() - 40), height = 18;
        const int stops = static_cast<int>(sharedRamp.stops.size());
        for (int x = 0; x < width; ++x) {
            const double t = static_cast<double>(x) / qMax(1, width - 1) * (stops - 1);
            const int i = qMin(stops - 2, static_cast<int>(t));
            drawSolidRect(canvas, left + x, top, 1, height,
                interpolateColor(sharedRamp.stops[i], sharedRamp.stops[i + 1], t - i).rgb());
        }
        // Five evenly spaced physical ticks, so the numbers always describe the
        // ramp that was actually drawn.
        for (int tick = 0; tick <= 4; ++tick) {
            const double value = sharedRamp.minimum +
                (sharedRamp.maximum - sharedRamp.minimum) * tick / 4.0;
            QString text;
            if (fileName.contains(QStringLiteral("_CTH_")))
                text = QString::number(qRound(value / 1000.0));
            else if (fileName.contains(QStringLiteral("_PRE_")))
                text = QString::number(value, 'f', 2);
            else if (fileName.contains(QStringLiteral("_OLR_")))
                text = QString::number(qRound(value));
            else if (fileName.contains(QStringLiteral("_LST_")) ||
                     fileName.contains(QStringLiteral("_SST_")) ||
                     fileName.contains(QStringLiteral("_CTT_")))
                text = QString::number(qRound(value));
            else
                text = QString::number(qRound(value));
            drawAscii(canvas, left + width * tick / 4 - 10, top + height + 3, text, ink, 1);
        }
        drawAscii(canvas, left + width - 34, top + height + 3, unitSuffix, ink, 1);
        return canvas;
    }
    if (fileName.contains(QStringLiteral("Region_VIS_Reflectance_Enhanced"))) {
        QImage canvas(map.width(), map.height() + 74, QImage::Format_RGB32);
        canvas.fill(QColor(12, 18, 25));
        const QImage grayMap = map.convertToFormat(QImage::Format_Grayscale8);
        for (int y = 0; y < grayMap.height(); ++y) {
            const uchar* src = grayMap.constScanLine(y);
            auto* dst = reinterpret_cast<QRgb*>(canvas.scanLine(y));
            for (int x = 0; x < grayMap.width(); ++x) dst[x] = qRgb(src[x], src[x], src[x]);
        }
        const QRgb ink = qRgb(235, 240, 246);
        drawAscii(canvas, 18, map.height() + 7,
            QStringLiteral("VIS ENHANCED - BRIGHTER=HIGHER SENSOR REFLECTANCE; NO ATMOSPHERIC CORRECTION"), ink);
        const int left = 20, top = map.height() + 39, width = qMin(460, map.width() - 40), height = 18;
        for (int x = 0; x < width; ++x) {
            const int shade = qRound(255.0 * x / qMax(1, width - 1));
            drawSolidRect(canvas, left + x, top, 1, height, qRgb(shade, shade, shade));
        }
        drawAscii(canvas, left, top + height + 3, QStringLiteral("LOW"), ink, 1);
        drawAscii(canvas, left + width - 35, top + height + 3, QStringLiteral("HIGH"), ink, 1);
        return canvas;
    } else if (fileName.contains(QStringLiteral("_TBB_Enhanced"))) {
        QImage canvas(map.width(), map.height() + 74, QImage::Format_RGB32);
        canvas.fill(QColor(12, 18, 25));
        const QImage rgbMap = map.convertToFormat(QImage::Format_RGB32);
        for (int y = 0; y < map.height(); ++y)
            std::memcpy(canvas.scanLine(y), rgbMap.constScanLine(y), static_cast<std::size_t>(map.width()) * 4);
        const QRgb ink = qRgb(235, 240, 246);
        drawAscii(canvas, 18, map.height() + 7,
            QStringLiteral("BRIGHTNESS TEMP - COLD CLOUDS=BRIGHT; WARMER SURFACE=DARK; HOVER FOR K/ C"), ink);
        const int left = 20, top = map.height() + 39, width = qMin(460, map.width() - 40), height = 18;
        for (int x = 0; x < width; ++x) {
            const int shade = qRound(255.0 * x / qMax(1, width - 1));
            drawSolidRect(canvas, left + x, top, 1, height, qRgb(shade, shade, shade));
        }
        drawAscii(canvas, left, top + height + 3, QStringLiteral("320K WARM"), ink, 1);
        drawAscii(canvas, left + width - 48, top + height + 3, QStringLiteral("180K COLD"), ink, 1);
        return canvas;
    } else if (fileName.contains(QStringLiteral("_CTT_Enhanced"))) {
        QImage canvas(map.width(), map.height() + 74, QImage::Format_RGB32);
        canvas.fill(QColor(12, 18, 25));
        const QImage rgbMap = map.convertToFormat(QImage::Format_RGB32);
        for (int y = 0; y < map.height(); ++y)
            std::memcpy(canvas.scanLine(y), rgbMap.constScanLine(y), static_cast<std::size_t>(map.width()) * 4);
        const QRgb ink = qRgb(235, 240, 246);
        drawAscii(canvas, 18, map.height() + 7,
            QStringLiteral("CTT IR1 CLOUD-TOP TEMPERATURE CANDIDATE - GRAY=NO CLOUD; NOT CLOUD HEIGHT"), ink);
        const int left = 20, top = map.height() + 39, width = qMin(460, map.width() - 40), height = 18;
        const QVector<QColor> stops{QColor(45, 80, 220), QColor(45, 205, 235),
            QColor(55, 190, 105), QColor(245, 220, 55), QColor(245, 145, 45), QColor(220, 50, 55)};
        for (int x = 0; x < width; ++x) {
            const double t = static_cast<double>(x) / qMax(1, width - 1) * (stops.size() - 1);
            const int i = qMin(static_cast<int>(stops.size()) - 2, static_cast<int>(t));
            const QColor color = interpolateColor(stops[i], stops[i + 1], t - i);
            drawSolidRect(canvas, left + x, top, 1, height, color.rgb());
        }
        for (int kelvin : {180, 220, 260, 300, 320})
            drawAscii(canvas, left + width * (kelvin - 180) / 140 - 10, top + height + 3,
                QStringLiteral("%1K").arg(kelvin), ink, 1);
        return canvas;
    } else if (fileName.contains(QStringLiteral("_PRE_Enhanced")) ||
               fileName.contains(QStringLiteral("_UTH_Enhanced"))) {
        const bool isPre = fileName.contains(QStringLiteral("_PRE_Enhanced"));
        const int low = 0, high = isPre ? 0 : 100;
        const QString caption = isPre
            ? QStringLiteral("PRE 1-HOUR CALIBRATED ESTIMATE; MM")
            : QStringLiteral("UTH CALIBRATED ESTIMATE; RELATIVE HUMIDITY PERCENT");
        const QVector<QColor> stops = isPre
            ? QVector<QColor>{QColor(35, 70, 170), QColor(30, 170, 210), QColor(65, 190, 110), QColor(245, 210, 55), QColor(225, 75, 45)}
            : QVector<QColor>{QColor(40, 70, 170), QColor(35, 175, 220), QColor(90, 210, 150), QColor(240, 220, 90), QColor(220, 110, 70)};
        QImage canvas(map.width(), map.height() + 78, QImage::Format_RGB32);
        canvas.fill(QColor(12, 18, 25));
        const QImage rgbMap = map.convertToFormat(QImage::Format_RGB32);
        for (int y = 0; y < map.height(); ++y)
            std::memcpy(canvas.scanLine(y), rgbMap.constScanLine(y), static_cast<std::size_t>(map.width()) * 4);
        const QRgb ink = qRgb(235, 240, 246);
        drawAscii(canvas, 18, map.height() + 7, caption, ink);
        const int left = 20, top = map.height() + 39, width = qMin(460, map.width() - 40), height = 18;
        for (int x = 0; x < width; ++x) {
            const double t = static_cast<double>(x) / qMax(1, width - 1) * (stops.size() - 1);
            const int i = qMin(static_cast<int>(stops.size()) - 2, static_cast<int>(t));
            drawSolidRect(canvas, left + x, top, 1, height,
                interpolateColor(stops[i], stops[i + 1], t - i).rgb());
        }
        for (int tick = 0; tick <= 4; ++tick) {
            const double value = isPre ? tick * 0.075 : static_cast<double>(low + (high - low) * tick / 4);
            drawAscii(canvas, left + width * tick / 4 - 10, top + height + 3,
                QString::number(value, 'f', isPre ? 2 : 0), ink, 1);
        }
        return canvas;
    } else if (fileName.contains(QStringLiteral("_OLR_Enhanced"))) {
        QImage canvas(map.width(), map.height() + 78, QImage::Format_RGB32);
        canvas.fill(QColor(12, 18, 25));
        const QImage rgbMap = map.convertToFormat(QImage::Format_RGB32);
        for (int y = 0; y < map.height(); ++y)
            std::memcpy(canvas.scanLine(y), rgbMap.constScanLine(y), static_cast<std::size_t>(map.width()) * 4);
        const QRgb ink = qRgb(235, 240, 246);
        const bool calibrated = fileName.contains(QStringLiteral("Region_OLR_Enhanced"));
        drawAscii(canvas, 18, map.height() + 7,
            calibrated ? QStringLiteral("OLR EMPIRICALLY CALIBRATED AGAINST FY-2G OFFICIAL PRODUCT; W/M2")
                       : QStringLiteral("OLR PROXY W/M2 - SIGMA T^4 FROM IR1 TBB; NOT BROADBAND FLUX"), ink);
        const int left = 20, top = map.height() + 39, width = qMin(460, map.width() - 40), height = 18;
        const QVector<QColor> stops{QColor(35, 70, 170), QColor(30, 170, 210),
            QColor(65, 190, 110), QColor(245, 210, 55), QColor(225, 75, 45)};
        for (int x = 0; x < width; ++x) {
            const double t = static_cast<double>(x) / qMax(1, width - 1) * (stops.size() - 1);
            const int i = qMin(static_cast<int>(stops.size()) - 2, static_cast<int>(t));
            drawSolidRect(canvas, left + x, top, 1, height,
                interpolateColor(stops[i], stops[i + 1], t - i).rgb());
        }
        const int scaleLow = calibrated ? 40 : 50;
        const int scaleHigh = calibrated ? 420 : 550;
        const QVector<int> ticks = calibrated ? QVector<int>{40, 135, 230, 325, 420}
                                               : QVector<int>{50, 150, 250, 350, 450, 550};
        for (int value : ticks)
            drawAscii(canvas, left + width * (value - scaleLow) / (scaleHigh - scaleLow) - 10, top + height + 3,
                QString::number(value), ink, 1);
        return canvas;
    } else if (fileName.contains(QStringLiteral("_SST_Enhanced"))) {
        const int low = 270, high = 310;
        const QString caption = QStringLiteral("SST CLEAR-OCEAN SPLIT-WINDOW CANDIDATE; K");
        const QVector<QColor> stops{QColor(35, 70, 170), QColor(30, 170, 210), QColor(65, 190, 110), QColor(245, 210, 55), QColor(225, 75, 45)};
        QImage canvas(map.width(), map.height() + 78, QImage::Format_RGB32);
        canvas.fill(QColor(12, 18, 25));
        const QImage rgbMap = map.convertToFormat(QImage::Format_RGB32);
        for (int y = 0; y < map.height(); ++y)
            std::memcpy(canvas.scanLine(y), rgbMap.constScanLine(y), static_cast<std::size_t>(map.width()) * 4);
        const QRgb ink = qRgb(235, 240, 246);
        drawAscii(canvas, 18, map.height() + 7, caption, ink);
        const int left = 20, top = map.height() + 39, width = qMin(460, map.width() - 40), height = 18;
        for (int x = 0; x < width; ++x) {
            const double t = static_cast<double>(x) / qMax(1, width - 1) * (stops.size() - 1);
            const int i = qMin(static_cast<int>(stops.size()) - 2, static_cast<int>(t));
            drawSolidRect(canvas, left + x, top, 1, height,
                interpolateColor(stops[i], stops[i + 1], t - i).rgb());
        }
        for (int tick = 0; tick <= 4; ++tick) {
            const int value = low + (high - low) * tick / 4;
            drawAscii(canvas, left + width * tick / 4 - 10, top + height + 3,
                QString::number(value), ink, 1);
        }
        return canvas;
    } else if (fileName.contains(QStringLiteral("_CTH_Enhanced")) ||
               fileName.contains(QStringLiteral("_CTH_IR1"))) {
        QImage canvas(map.width(), map.height() + 78, QImage::Format_RGB32);
        canvas.fill(QColor(12, 18, 25));
        const QImage rgbMap = map.convertToFormat(QImage::Format_RGB32);
        for (int y = 0; y < map.height(); ++y)
            std::memcpy(canvas.scanLine(y), rgbMap.constScanLine(y), static_cast<std::size_t>(map.width()) * 4);
        const QRgb ink = qRgb(235, 240, 246);
        drawAscii(canvas, 18, map.height() + 7,
            QStringLiteral("CTH IR1 CLOUD-TOP HEIGHT CANDIDATE - STANDARD ATMOSPHERE INVERSION; LOW=WARM TOP"), ink);
        const int left = 20, top = map.height() + 39, width = qMin(460, map.width() - 40), height = 18;
        const QVector<QColor> stops{QColor(35, 70, 170), QColor(30, 170, 210),
            QColor(65, 190, 110), QColor(245, 210, 55), QColor(225, 75, 45)};
        for (int x = 0; x < width; ++x) {
            const double t = static_cast<double>(x) / qMax(1, width - 1) * (stops.size() - 1);
            const int i = qMin(static_cast<int>(stops.size()) - 2, static_cast<int>(t));
            drawSolidRect(canvas, left + x, top, 1, height,
                interpolateColor(stops[i], stops[i + 1], t - i).rgb());
        }
        for (int metres : {0, 4000, 8000, 12000, 16000, 20000})
            drawAscii(canvas, left + width * metres / 20000 - 10, top + height + 3,
                QStringLiteral("%1").arg(metres / 1000), ink, 1);
        drawAscii(canvas, left + width - 40, top + height + 3, QStringLiteral("km"), ink, 1);
        return canvas;
    } else if (fileName.contains(QStringLiteral("_FOG_"))) {
        QImage canvas(map.width(), map.height() + 78, QImage::Format_RGB32);
        canvas.fill(QColor(12, 18, 25));
        const QImage rgbMap = map.convertToFormat(QImage::Format_RGB32);
        for (int y = 0; y < map.height(); ++y)
            std::memcpy(canvas.scanLine(y), rgbMap.constScanLine(y), static_cast<std::size_t>(map.width()) * 4);
        const QRgb ink = qRgb(235, 240, 246);
        drawAscii(canvas, 18, map.height() + 7,
            QStringLiteral("FOG / LOW STRATUS CANDIDATE CONFIDENCE - SPLIT WINDOW + WARM CLOUD TOP"), ink);
        const int left = 20, top = map.height() + 39, width = qMin(460, map.width() - 40), height = 18;
        for (int x = 0; x < width; ++x) {
            const double t = static_cast<double>(x) / qMax(1, width - 1);
            drawSolidRect(canvas, left + x, top, 1, height,
                interpolateColor(QColor(25, 45, 80), QColor(255, 70, 170), t).rgb());
        }
        for (int percent : {0, 25, 50, 75, 100})
            drawAscii(canvas, left + width * percent / 100 - 10, top + height + 3,
                QString::number(percent), ink, 1);
        drawAscii(canvas, left + width - 30, top + height + 3, QStringLiteral("%"), ink, 1);
        return canvas;
    } else if (fileName.contains(QStringLiteral("_LST_"))) {
        QImage canvas(map.width(), map.height() + 78, QImage::Format_RGB32);
        canvas.fill(QColor(12, 18, 25));
        const QImage rgbMap = map.convertToFormat(QImage::Format_RGB32);
        for (int y = 0; y < map.height(); ++y)
            std::memcpy(canvas.scanLine(y), rgbMap.constScanLine(y), static_cast<std::size_t>(map.width()) * 4);
        const QRgb ink = qRgb(235, 240, 246);
        drawAscii(canvas, 18, map.height() + 7,
            QStringLiteral("LST CLEAR-SKY SPLIT-WINDOW CANDIDATE - K; NO LAND MASK OR EMISSIVITY CORRECTION"), ink);
        const int left = 20, top = map.height() + 39, width = qMin(460, map.width() - 40), height = 18;
        const QVector<QColor> stops{QColor(35, 70, 170), QColor(30, 170, 210),
            QColor(65, 190, 110), QColor(245, 210, 55), QColor(225, 75, 45)};
        for (int x = 0; x < width; ++x) {
            const double t = static_cast<double>(x) / qMax(1, width - 1) * (stops.size() - 1);
            const int i = qMin(static_cast<int>(stops.size()) - 2, static_cast<int>(t));
            drawSolidRect(canvas, left + x, top, 1, height,
                interpolateColor(stops[i], stops[i + 1], t - i).rgb());
        }
        for (int kelvin : {260, 280, 300, 320, 340})
            drawAscii(canvas, left + width * (kelvin - 260) / 80 - 10, top + height + 3,
                QStringLiteral("%1K").arg(kelvin), ink, 1);
        return canvas;
    } else if (fileName.contains(QStringLiteral("Enhanced")) &&
               (fileName.contains(QStringLiteral("_CLD_")) ||
                fileName.contains(QStringLiteral("_CLC_")) ||
                fileName.contains(QStringLiteral("_CFR_")))) {
        // Coloured display for a categorical product. This file is a picture: the
        // numeric raster of the same family carries the codes and is written
        // separately, so applying a palette here cannot cost any data.
        QImage canvas(map.width(), map.height() + 78, QImage::Format_RGB32);
        canvas.fill(QColor(12, 18, 25));
        const bool isCld = fileName.contains(QStringLiteral("_CLD_"));
        const bool isCfr = fileName.contains(QStringLiteral("_CFR_"));
        // Read the index bytes directly when the input is indexed.
        //
        // convertToFormat(Grayscale16) must NOT be used here: for an indexed image
        // it expands the colour table, so index 1 becomes 61680 and every pixel
        // falls into the invalid arm, painting the whole map grey. That is the same
        // palette-expansion behaviour this file has already been bitten by twice.
        const bool indexed = map.format() == QImage::Format_Indexed8;
        const QImage src = indexed ? QImage() : map.convertToFormat(QImage::Format_Grayscale16);
        for (int y = 0; y < map.height(); ++y) {
            const uchar* irow = indexed ? map.constScanLine(y) : nullptr;
            const auto* row = indexed
                ? nullptr : reinterpret_cast<const quint16*>(src.constScanLine(y));
            auto* dst = reinterpret_cast<QRgb*>(canvas.scanLine(y));
            for (int x = 0; x < map.width(); ++x) {
                const int v = indexed ? int(irow[x]) : int(row[x]);
                if (isCfr) {
                    // 0-100 percent; 255 means no valid samples. No-data is drawn
                    // DARK rather than mid-grey, because mid-grey sits in the middle
                    // of the percentage ramp and the off-disk area (55.6 % of the
                    // frame on the reference scene) then reads as "about half
                    // cloudy". Dark is outside the ramp, as for the cloud mask.
                    const int pct = v / 257;
                    if (pct >= 100) dst[x] = qRgb(18, 22, 28);
                    else {
                        const int shade = qBound(0, qRound(pct * 2.55), 255);
                        dst[x] = qRgb(shade, shade, shade);
                    }
                    continue;
                }
                if (isCld) {
                    if (v == 0) dst[x] = qRgb(30, 38, 46);        // clear
                    else if (v == 1) dst[x] = qRgb(240, 240, 240); // cloud candidate
                    else dst[x] = qRgb(115, 125, 138);             // invalid
                    continue;
                }
                switch (v) {   // CLC class codes
                case 0: dst[x] = qRgb(40, 60, 90); break;
                case 1: dst[x] = qRgb(240, 240, 240); break;
                case 2: dst[x] = qRgb(90, 150, 230); break;
                case 3: dst[x] = qRgb(160, 200, 240); break;
                case 4: dst[x] = qRgb(240, 200, 90); break;
                default: dst[x] = qRgb(115, 125, 138); break;
                }
            }
        }
        const QRgb ink = qRgb(235, 240, 246);
        const QString caption = isCld
            ? QStringLiteral("CLD CLOUD MASK - WHITE=CLOUD CANDIDATE; DARK=CLEAR; GREY=NO DATA")
            : (isCfr
                   ? QStringLiteral("CFR CLOUD FRACTION - GREY 0-100 PCT; GREY=NO DATA")
                   : QStringLiteral("CLC CLOUD CLASS - COLOURS MAP TO CLASS IN THE LEGEND"));
        drawAscii(canvas, 18, map.height() + 8, caption, ink);
        const int left = 20, top = map.height() + 39, width = qMin(460, map.width() - 40), height = 18;
        if (isCld || isCfr) {
            for (int x = 0; x < width; ++x) {
                const int shade = qRound(255.0 * x / qMax(1, width - 1));
                drawSolidRect(canvas, left + x, top, 1, height, qRgb(shade, shade, shade));
            }
            if (isCfr) {
                for (int value : {0, 25, 50, 75, 100})
                    drawAscii(canvas, left + width * value / 100 - 8, top + height + 3,
                              QString::number(value), ink, 1);
            } else {
                drawAscii(canvas, left, top + height + 3, QStringLiteral("CLEAR"), ink, 1);
                drawAscii(canvas, left + width - 44, top + height + 3, QStringLiteral("CLOUD"), ink, 1);
            }
        } else {
            const struct { const char* text; QRgb colour; } items[] = {
                {"CLEAR", qRgb(40, 60, 90)}, {"CLOUD", qRgb(240, 240, 240)},
                {"HIGH", qRgb(90, 150, 230)}, {"MID/LOW", qRgb(160, 200, 240)},
                {"THIN CIRRUS", qRgb(240, 200, 90)}};
            int cx = left;
            for (const auto& item : items) {
                drawSolidRect(canvas, cx, top - 4, 16, 16, item.colour);
                drawAscii(canvas, cx + 20, top, QString::fromLatin1(item.text), ink, 1);
                cx += 88;
            }
        }
        return canvas;
    } else if (fileName.contains(QStringLiteral("_CLC_"))) {
        labels = {QStringLiteral("0 CLEAR"), QStringLiteral("1 CLOUD"), QStringLiteral("2 HIGH"),
                  QStringLiteral("3 MID/LOW"), QStringLiteral("4 CIRRUS"), QStringLiteral("255 NODATA")};
        colors = {QColor(30, 38, 46), QColor(230, 230, 230), QColor(200, 95, 220),
                  QColor(245, 180, 55), QColor(70, 205, 235), QColor(115, 125, 138)};
    } else if (fileName.contains(QStringLiteral("_CLD_"))) {
        labels = {QStringLiteral("0 CLEAR"), QStringLiteral("1 CLOUD"), QStringLiteral("255 NODATA")};
        colors = {QColor(30, 38, 46), QColor(240, 240, 240), QColor(115, 125, 138)};

    } else if (fileName.contains(QStringLiteral("_CFR_"))) {
        {
            int lo = 1 << 20, hi = -1, n255 = 0;
            for (int y = 0; y < map.height(); y += qMax(1, map.height() / 100)) {
                const uchar* row = map.constScanLine(y);
                for (int x = 0; x < map.width(); x += qMax(1, map.width() / 100)) {
                    const int v = (map.format() == QImage::Format_Grayscale16)
                        ? reinterpret_cast<const quint16*>(row)[x] : int(row[x]);
                    lo = qMin(lo, v);
                    hi = qMax(hi, v);
                    if (v == 255) ++n255;
                }
            }
            std::cerr << "[probe] CFR entering legend: format=" << int(map.format())
                      << " min=" << lo << " max=" << hi << " n255=" << n255 << "\n";
        }
        QImage canvas(map.width(), map.height() + 78, QImage::Format_RGB32);
        canvas.fill(QColor(12, 18, 25));
        // The fraction goes to the footer canvas at its own scale.
        //
        // This branch used to stretch the percentages to 0..255 (src * 255 / 100)
        // so the map looked contrasty. That is acceptable for a picture but not for
        // a measurement raster: saveProduct() stores the grey back into 16 bits, so
        // the file held pct * 655 / 100 -- a "cloud fraction" of 191% and 227% on
        // real scenes while the sidecar declared 0-100. Copied through unchanged.
        const QImage fraction = map.convertToFormat(QImage::Format_Grayscale8);
        {
            int lo = 1 << 20, hi = -1;
            for (int y = 0; y < fraction.height(); y += qMax(1, fraction.height() / 100)) {
                const uchar* row = fraction.constScanLine(y);
                for (int x = 0; x < fraction.width(); x += qMax(1, fraction.width() / 100)) {
                    lo = qMin(lo, int(row[x]));
                    hi = qMax(hi, int(row[x]));
                }
            }
            std::cerr << "[probe] CFR after convertToFormat(Gray8): min=" << lo
                      << " max=" << hi << "\n";
        }
        for (int y = 0; y < fraction.height(); ++y) {
            const uchar* src = fraction.constScanLine(y);
            auto* dst = reinterpret_cast<QRgb*>(canvas.scanLine(y));
            for (int x = 0; x < fraction.width(); ++x) {
                // The nodata sentinel must be the value the sidecar advertises
                // (255), not the on-screen grey it used to be. qRgb(115,125,138)
                // became qGray 124 on the way into the 16-bit file, so the saved
                // sentinel was 124 while the metadata promised 255.
                if (src[x] == 255) dst[x] = qRgb(255, 255, 255);
                else dst[x] = qRgb(src[x], src[x], src[x]);        // value as-is
            }
        }
        const QRgb ink = qRgb(235, 240, 246);
        drawAscii(canvas, 18, map.height() + 8, QStringLiteral("CFR CLOUD FRACTION - GRAYSCALE 0-100 PCT; GRAY=NODATA"), ink);
        const int left = 20, top = map.height() + 39, width = qMin(460, map.width() - 40), height = 18;
        for (int x = 0; x < width; ++x) {
            const int shade = qRound(255.0 * x / qMax(1, width - 1));
            drawSolidRect(canvas, left + x, top, 1, height, qRgb(shade, shade, shade));
        }
        for (int value : {0, 25, 50, 75, 100})
            drawAscii(canvas, left + width * value / 100 - 8, top + height + 3, QString::number(value), ink, 1);
        return canvas;
    } else {
        return map;
    }

    constexpr int footerHeight = 74;
    QImage canvas(map.width(), map.height() + footerHeight, QImage::Format_RGB32);
    canvas.fill(QColor(12, 18, 25));
    // Colour products are copied through RGB32, which is fine for them.
    //
    // An indexed raster must NOT go that way: convertToFormat(RGB32) applies the
    // palette, so the cloud mask's codes 0/1/255 arrive here as greys derived from
    // qRgb(30,38,46), qRgb(240,240,240) and qRgb(115,125,138) -- 9546 / 61678 /
    // 31861 in the saved file. Verified with work/fy2verify/indexed_probe.cpp.
    // The indices are therefore written straight into the grey channel, which
    // keeps codes 0/1/255 intact and still lets the legend be drawn on top.
    const QImage source =
        (map.format() == QImage::Format_Indexed8)
            ? [&map] {
                  QImage gray(map.size(), QImage::Format_RGB32);
                  for (int y = 0; y < map.height(); ++y) {
                      const uchar* src = map.constScanLine(y);
                      auto* dst = reinterpret_cast<QRgb*>(gray.scanLine(y));
                      // Scale 0..255 unchanged: the code IS the value here, and
                      // 255 is an invalid sentinel rather than a brightness.
                      for (int x = 0; x < map.width(); ++x) {
                          const int v = src[x];
                          dst[x] = qRgb(v, v, v);
                      }
                  }
                  return gray;
              }()
            : map.convertToFormat(QImage::Format_RGB32);
    for (int y = 0; y < map.height(); ++y)
        std::memcpy(canvas.scanLine(y), source.constScanLine(y),
                    static_cast<std::size_t>(map.width()) * 4);
    const QRgb ink = qRgb(235, 240, 246);
    drawAscii(canvas, 18, map.height() + 7,
        fileName.contains(QStringLiteral("_CLC_"))
            ? QStringLiteral("CLC CLOUD CLASS - COLORS MAP TO CLOUD CLASS")
            : QStringLiteral("CLD CLOUD MASK - LIGHT=CLOUD; DARK=CLEAR; GRAY=NODATA"), ink);
    const int itemWidth = qMax(130, (canvas.width() - 36) / qMax(1, labels.size()));
    int x = 18;
    const int y = map.height() + 42;
    for (int i = 0; i < labels.size(); ++i) {
        drawSolidRect(canvas, x, y - 11, 18, 18, colors[i].rgb());
        drawAscii(canvas, x + 25, y - 8, labels[i], ink, 2);
        x += itemWidth;
    }
    return canvas;
}

}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QImageReader::setAllocationLimit(1024);
    if (argc < 4) {
        std::cerr << "Usage: NadirStation_RegionalProducts observation-folder center-lat center-lon [norad-id [projected-shift-x projected-shift-y gcp-lat gcp-lon gcp-pixel-x gcp-pixel-y]]\n";
        return 2;
    }
    const QDir directory(QString::fromLocal8Bit(argv[1]));
    bool latOk = false, lonOk = false;
    const double lat = QString::fromLocal8Bit(argv[2]).toDouble(&latOk);
    const double lon = QString::fromLocal8Bit(argv[3]).toDouble(&lonOk);
    const QString norad = argc >= 5 ? QString::fromLocal8Bit(argv[4]) : QStringLiteral("40367");
    int projectedOffsetX = 0, projectedOffsetY = 0, gcpPixelX = 0, gcpPixelY = 0;
    double gcpLatitude = 0.0, gcpLongitude = 0.0;
    if (argc >= 11) {
        bool okX = false, okY = false, okLat = false, okLon = false, okPixelX = false, okPixelY = false;
        projectedOffsetX = QString::fromLocal8Bit(argv[5]).toInt(&okX);
        projectedOffsetY = QString::fromLocal8Bit(argv[6]).toInt(&okY);
        gcpLatitude = QString::fromLocal8Bit(argv[7]).toDouble(&okLat);
        gcpLongitude = QString::fromLocal8Bit(argv[8]).toDouble(&okLon);
        gcpPixelX = QString::fromLocal8Bit(argv[9]).toInt(&okPixelX);
        gcpPixelY = QString::fromLocal8Bit(argv[10]).toInt(&okPixelY);
        if (!okX || !okY || !okLat || !okLon || !okPixelX || !okPixelY) {
            std::cerr << "Invalid GCP calibration values.\n";
            return 3;
        }
    }
    if (!directory.exists() || !latOk || !lonOk) {
        std::cerr << "Invalid directory or center coordinates.\n";
        return 3;
    }
    QString observationPath = directory.filePath(QStringLiteral("FY2_Observation.json"));
    if (!QFileInfo::exists(observationPath)) {
        const QString fy2gPath = directory.filePath(QStringLiteral("FY2G_Observation.json"));
        const QString fy2hPath = directory.filePath(QStringLiteral("FY2H_Observation.json"));
        if (QFileInfo::exists(fy2gPath)) observationPath = fy2gPath;
        else if (QFileInfo::exists(fy2hPath)) observationPath = fy2hPath;
        else observationPath = directory.filePath(QStringLiteral("MGAS_Observation.json")); // legacy input
    }
    QFile manifestFile(observationPath);
    if (!manifestFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::cerr << "Observation manifest is missing.\n";
        return 4;
    }
    QJsonObject manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
    manifestFile.close();
    QDateTime epoch = observationMidpoint(directory.absolutePath(), manifest);
    nadirstation::orbit::TleRecord tle;
    nadirstation::orbit::SatellitePosition raw, camera;
    QString error;
    if (!epoch.isValid() || !nadirstation::orbit::loadTle(norad, tle, &error) ||
        !nadirstation::orbit::propagate(tle, epoch.toUTC(), raw, &error)) {
        std::cerr << "TLE position unavailable: " << error.toLocal8Bit().constData() << '\n';
        return 5;
    }
    camera = norad == QStringLiteral("40367")
        ? nadirstation::orbit::calibrateFy2gCameraBoresight(raw) : raw;
    // Per-observation navigation (TLE per scan line, orbit-normal scan axis, this scene's IR1 disk,
    // coastline-validated residual). When it is available the true mid-scan satellite position
    // replaces the equatorial "camera" everywhere (view zenith, sidecars), and the epoch becomes the
    // mid-scan time: the folder name is the recording start and can precede the image by 30-50 min.
    QString navigationReport;
    const nadirstation::products::ScanGeometry scan =
        nadirstation::products::navigateObservation(norad, directory.absolutePath(), &navigationReport);
    std::cerr << "[regional] " << navigationReport.toLocal8Bit().constData() << '\n';
    if (scan.navigation.valid) {
        const double midLine = 0.5 * (scan.navigation.scanLineMin + scan.navigation.scanLineMax);
        camera = nadirstation::products::navigationSatelliteAtLine(scan.navigation, midLine);
        raw = camera;
        epoch = scan.navigation.scanStartUtc.addMSecs(
            scan.navigation.scanStartUtc.msecsTo(scan.navigation.scanEndUtc) / 2);
        g_navigationStatus = scan.navigation.summary;
    } else {
        g_navigationStatus = navigationReport + QStringLiteral(
            "; camera assumed Earth-pointing/equatorial at the TLE longitude. Expect 10-40 px errors.");
    }
    const QString satellite = norad == QStringLiteral("40367") ? QStringLiteral("FY-2G") : QStringLiteral("FY-2H");
    // Keep the session identity in sync with the spacecraft ID used for geometry.
    // Older receiver builds wrote every S-VISSR session as FY-2G, including 2H.
    manifest[QStringLiteral("satellite")] = satellite;
    manifest[QStringLiteral("instrument")] = satellite + QStringLiteral(" S-VISSR");
    const nadirstation::orbit::ScanCalibration scanCalibration = nadirstation::orbit::scanCalibrationFor(norad);
    std::cerr << "[regional] scan geometry: stepScale=" << scan.stepScale
              << " residual=(" << scan.residualRowPx << ", " << scan.residualColPx
              << ") px; TLE subpoint " << camera.subpointLongitudeDeg << " E\n";
    QJsonArray products = manifest.value("products").toArray();
    const QStringList obsoleteFiles{
        QStringLiteral("Region_CH2_IR4_Display.png"), QStringLiteral("Region_CH3_IR3_Display.png"),
        QStringLiteral("Region_CH4_IR1_Display.png"), QStringLiteral("Region_CH5_IR2_Display.png"),
        QStringLiteral("Region_IR_Enhanced_Display.png"), QStringLiteral("Region_CLD_IR1_Threshold.png"),
        QStringLiteral("Region_CTT_IR1_0p01K.png"), QStringLiteral("Region_CLC_IR1_ThermalClass.png"),
        QStringLiteral("Region_CFR_32px_Threshold.png"), QStringLiteral("Region_MCIR_GCP.png"),
        QStringLiteral("China_Projected_MCIR.png"), QStringLiteral("China_Projected_MCIR.json"),
        QStringLiteral("CLD_IR1_threshold_proxy.png"),
        QStringLiteral("CTT_cloud_only_0p01K.png"),
        QStringLiteral("Region_PRE_ConvectivePotentialIndex.png"),
        QStringLiteral("Region_UTH_WaterVaporProxyIndex.png"),
        QStringLiteral("Region_CFR_CTAAmountCandidate_0p01pct.png"),
        QStringLiteral("Region_CFR_CTAAmountCandidate_Enhanced.png"),
        QStringLiteral("Region_OLR_Calibrated_0p1Wm2.png"), QStringLiteral("Region_OLR_Enhanced.png"),
        QStringLiteral("Region_SST_LinearSpatialCV_0p01K.png"), QStringLiteral("Region_SST_LinearSpatialCV_Enhanced.png"),
        QStringLiteral("Region_PRE_Calibrated_0p01mm.png"), QStringLiteral("Region_PRE_Enhanced.png"),
        QStringLiteral("Region_UTH_Calibrated_1pct.png"), QStringLiteral("Region_UTH_Enhanced.png"),
        QStringLiteral("Region_SST_ClearOceanSplitWindowProxy_0p01K.png"), QStringLiteral("Region_SST_Candidate_Enhanced.png")};
    for (const QString& obsolete : obsoleteFiles) {
        QFile::remove(directory.filePath(obsolete));
        QFile::remove(directory.filePath(obsolete + QStringLiteral(".json")));
    }
    QJsonArray cleanedProducts;
    for (const QJsonValue& value : products) {
        const QString path = value.toObject().value("path").toString();
        if (!obsoleteFiles.contains(path) && path != QStringLiteral("Region_MCIR_GCP.png.json") &&
            path != QStringLiteral("China_Projected_MCIR.json"))
            cleanedProducts.append(value);
    }
    products = cleanedProducts;
    auto input = [&](const QString& name) { return QImage(directory.filePath(name)); };
    auto saveDataProduct = [&](const QString& name, const QImage& source,
                               const QString& label, const QString& units,
                               const QString& method, const QJsonObject& details = QJsonObject{}) {
        if (source.isNull()) return false;
        int repairedRows = 0;
        const bool tbbRaster = name.contains(QStringLiteral("_TBB_0p01K"));
        const QImage repaired = tbbRaster
            ? nadirstation::products::repairMissingLinesMedian(source, 2, &repairedRows) : source;
        const QImage projectedBeforeRepair = nadirstation::products::chinaGeostationaryDataProjection(
            repaired, lat, lon, camera, 1, projectedOffsetX, projectedOffsetY, scan);
        int projectedRepairedRows = 0;
        const QImage projected = tbbRaster
            ? nadirstation::products::repairMissingLinesMedian(projectedBeforeRepair, 2, &projectedRepairedRows)
            : projectedBeforeRepair;
        QJsonObject metadata = details;
        metadata["projection_center_latitude_deg"] = lat;
        metadata["projection_center_longitude_deg"] = lon;
        metadata["projection"] = QStringLiteral("equirectangular / geographic longitude-latitude grid");
        if (tbbRaster) {
            metadata["missing_line_repair"] = QStringLiteral("detect short internal all-invalid or strongly anomalous rows before and after projection; interpolate numeric pixels between valid boundary scanlines");
            metadata["missing_rows_repaired"] = repairedRows;
            metadata["projected_missing_rows_repaired"] = projectedRepairedRows;
        }
        QImage numericProjected = projected;
        if (tbbRaster && projected.format() == QImage::Format_Grayscale16) {
            numericProjected = projected.copy();
            for (int y = 0; y < numericProjected.height(); ++y) {
                auto* row = reinterpret_cast<quint16*>(numericProjected.scanLine(y));
                for (int x = 0; x < numericProjected.width(); ++x)
                    if (row[x] == 0) row[x] = 65535;
            }
            metadata[QStringLiteral("no_data_value")] = 65535;
            metadata[QStringLiteral("encoding")] = QStringLiteral("uint16; value / 100 = kelvin; 65535 = no data");
        }
        bool savedProduct = saveProduct(directory, name, numericProjected, label, units, method,
            satellite, norad, raw, camera, epoch, products, projectedOffsetX, projectedOffsetY,
            gcpLatitude, gcpLongitude, gcpPixelX, gcpPixelY, metadata, isViewerProduct(name));
        if (tbbRaster && !projected.isNull()) {
            QImage display(projected.size(), QImage::Format_RGB32);
            constexpr int coldLimit = 18000, warmLimit = 32000; // 0.01 K units
            for (int y = 0; y < projected.height(); ++y) {
                const auto* src = reinterpret_cast<const quint16*>(projected.constScanLine(y));
                auto* dst = reinterpret_cast<QRgb*>(display.scanLine(y));
                for (int x = 0; x < projected.width(); ++x) {
                    const int value = src[x];
                    if (value < 15000 || value > 35000) dst[x] = qRgb(115, 125, 138);
                    else {
                        const int shade = qBound(0, qRound((warmLimit - value) * 255.0 /
                            (warmLimit - coldLimit)), 255);
                        dst[x] = qRgb(shade, shade, shade);
                    }
                }
            }
            const QString displayName = name;
            QString enhancedName = displayName;
            enhancedName.replace(QStringLiteral("_TBB_0p01K"), QStringLiteral("_TBB_Enhanced"));
            const QImage displayWithLegend = addProductLegend(display, enhancedName);
            QJsonObject displayMetadata = metadata;
            displayMetadata[QStringLiteral("display_of_numeric_product")] = name;
            displayMetadata[QStringLiteral("display_note")] = QStringLiteral("Grayscale stretch from 180 K to 320 K; colder pixels are brighter. Hover reads the preserved numeric TBB raster.");
            displayMetadata[QStringLiteral("temperature_range_kelvin")] = QJsonArray{180, 320};
            displayMetadata[QStringLiteral("display_layout")] = QStringLiteral("map on top; brightness-temperature legend in appended footer rows");
            savedProduct = saveProduct(directory, enhancedName, displayWithLegend,
                label + QStringLiteral(" 路 display"), QStringLiteral("grayscale display; hover reports calibrated K and 掳C"),
                method, satellite, norad, raw, camera, epoch, products, projectedOffsetX, projectedOffsetY,
                gcpLatitude, gcpLongitude, gcpPixelX, gcpPixelY, displayMetadata) || savedProduct;
        }
        return savedProduct;
    };
    auto saveProjectedDataProduct = [&](const QString& name, const QImage& source,
                                        const QString& label, const QString& units,
                                        const QString& method, QJsonObject metadata = QJsonObject{},
                                        bool sourceAlreadyProjected = false,
                                        const QString& displayName = QString()) {
        if (source.isNull()) return false;
        int repairedRows = 0;
        const QImage repaired = sourceAlreadyProjected
            ? source.copy() : nadirstation::products::repairMissingLinesMedian(source, 2, &repairedRows);
        metadata["projection_center_latitude_deg"] = lat;
        metadata["projection_center_longitude_deg"] = lon;
        metadata["projection"] = QStringLiteral("equirectangular / geographic longitude-latitude grid");
        metadata["missing_line_repair"] = sourceAlreadyProjected
            ? QStringLiteral("not applied; source raster is already on the regional grid and its values are preserved")
            : QStringLiteral("detect short internal invalid or strongly anomalous rows; interpolate continuous values and fill categorical values by local mode");
        metadata["missing_rows_repaired"] = repairedRows;
        metadata["source_already_projected"] = sourceAlreadyProjected;
        const QImage display = addProductLegend(repaired, name);
        metadata["display_layout"] = display.size() == repaired.size()
            ? QStringLiteral("native raster")
            : QStringLiteral("map on top; color/scale legend in appended footer rows");
        metadata["map_height_pixels"] = repaired.height();
        metadata["listed_in_image_viewer"] = isViewerProduct(name);
        // Bracket the legend call. addProductLegend() is documented to only add a
        // footer to display images, so for a numeric raster it should hand the
        // image straight back; but saveProduct() reports receiving RGB32 for the
        // cloud mask, which means something in between widens it. Printing the
        // format on both sides of the call settles which step does it, instead of
        // reading the function and inferring -- inference already produced two
        // wrong answers on this bug.
        std::cerr << "[regional] fmt before legend: " << int(repaired.format())
                  << " after legend: " << int(display.format())
                  << " sizes " << repaired.width() << "x" << repaired.height()
                  << " -> " << display.width() << "x" << display.height()
                  << "  (" << qPrintable(name) << ")\n";
        const bool numericOk = saveProduct(directory, name, display, label, units, method,
            satellite, norad, raw, camera, epoch, products, projectedOffsetX, projectedOffsetY,
            gcpLatitude, gcpLongitude, gcpPixelX, gcpPixelY, metadata, isViewerProduct(name));
        if (displayName.isEmpty()) return numericOk;
        // A categorical raster cannot carry a legend footer without losing its
        // index codes, because painting it onto an RGB32 canvas applies the palette.
        // The legend therefore goes into a separate file: this one is a picture,
        // named "Enhanced" so the Image viewer lists it, and it points back at the
        // numeric raster that holds the codes.
        QJsonObject displayMeta = metadata;
        displayMeta[QStringLiteral("display_of_numeric_product")] = name;
        displayMeta[QStringLiteral("listed_in_image_viewer")] = true;
        displayMeta[QStringLiteral("numeric_raster_note")] = QStringLiteral(
            "This is the legend view. The numeric values are in the file named in "
            "display_of_numeric_product; this file's colours are a display mapping.");
        const QImage legendImage = addProductLegend(repaired, displayName);
        displayMeta[QStringLiteral("display_layout")] =
            legendImage.size() == repaired.size()
                ? QStringLiteral("native raster")
                : QStringLiteral("map on top; color/scale legend in appended footer rows");
        const bool displayOk = saveProduct(directory, displayName, legendImage,
            label + QStringLiteral(" 路 legend view"), QStringLiteral("display colours; see the numeric raster for values"),
            method, satellite, norad, raw, camera, epoch, products, projectedOffsetX,
            projectedOffsetY, gcpLatitude, gcpLongitude, gcpPixelX, gcpPixelY,
            displayMeta, true);
        return numericOk && displayOk;
    };
    auto saveContinuousProxy = [&](const QString& name, const QString& displayName,
                                   const QImage& source, double scaleMinimum, double scaleMaximum,
                                   quint16 noData, const QString& label, const QString& units,
                                   const QString& method, QJsonObject metadata,
                                   double storedUnitsPerDisplayUnit = 1.0,
                                   bool sourceAlreadyProjected = false,
                                   QVector<QColor> stops = {}) {
        if (source.isNull()) return 0;
        // The numeric raster is still written for GIS/analysis use, but it is a
        // measurement image rather than a picture, so it is deliberately kept out
        // of the manifest product list that populates the Image viewer.
        QImage normalizedSource = source;
        const bool zeroIsMissing = noData == 0 &&
            (name.contains(QStringLiteral("_OLR_")) || name.contains(QStringLiteral("_SST_")));
        if (zeroIsMissing && source.format() == QImage::Format_Grayscale16) {
            normalizedSource = source.copy();
            for (int y = 0; y < normalizedSource.height(); ++y) {
                auto* row = reinterpret_cast<quint16*>(normalizedSource.scanLine(y));
                for (int x = 0; x < normalizedSource.width(); ++x)
                    if (row[x] == 0) row[x] = 65535;
            }
            noData = 65535;
            metadata[QStringLiteral("no_data_value")] = 65535;
            QString encoding = metadata.value(QStringLiteral("encoding")).toString();
            encoding.replace(QStringLiteral("0 = no data"), QStringLiteral("65535 = no data"));
            encoding.replace(QStringLiteral("zero = no data"), QStringLiteral("65535 = no data"));
            encoding.replace(QStringLiteral("zero = invalid"), QStringLiteral("65535 = invalid"));
            encoding.replace(QStringLiteral("65535 = land, cloud, or invalid"), QStringLiteral("65535 = land, cloud, or invalid"));
            metadata[QStringLiteral("encoding")] = encoding;
        }
        const bool numericSaved = saveProjectedDataProduct(name, normalizedSource, label, units, method,
                                                           metadata, sourceAlreadyProjected);
        if (!numericSaved) return 0;
        // Colourise from the in-memory raster. Reading the numeric file back from
        // disk used to be safe, but once numeric rasters also received a legend
        // footer the read-back returned the colour+legend image and the numeric
        // raster itself stopped being a measurement raster.
        const QImage numeric = normalizedSource.convertToFormat(QImage::Format_Grayscale16);
        ColorRamp ramp{scaleMinimum, scaleMaximum, stops};
        if (ramp.stops.size() < 2) ramp = colorRampFor(displayName);
        if (ramp.stops.size() < 2)
            ramp = {scaleMinimum, scaleMaximum,
                    {QColor(35, 70, 170), QColor(30, 170, 210), QColor(65, 190, 110),
                     QColor(245, 210, 55), QColor(225, 75, 45)}};
        QImage color(numeric.size(), QImage::Format_RGB32);
        for (int y = 0; y < numeric.height(); ++y) {
            const auto* src = reinterpret_cast<const quint16*>(numeric.constScanLine(y));
            auto* dst = reinterpret_cast<QRgb*>(color.scanLine(y));
            for (int x = 0; x < numeric.width(); ++x) {
                if (src[x] == noData || src[x] == 65535) {
                    dst[x] = qRgb(115, 125, 138);
                    continue;
                }
                dst[x] = rampColor(ramp, src[x] / qMax(1.0, storedUnitsPerDisplayUnit)).rgb();
            }
        }
        QJsonObject displayMetadata = metadata;
        displayMetadata[QStringLiteral("display_of_numeric_product")] = name;
        displayMetadata[QStringLiteral("display_layout")] = QStringLiteral("regional equirectangular map with appended scale legend");
        displayMetadata[QStringLiteral("map_height_pixels")] = color.height();
        displayMetadata[QStringLiteral("display_scale_minimum")] = ramp.minimum;
        displayMetadata[QStringLiteral("display_scale_maximum")] = ramp.maximum;
        displayMetadata[QStringLiteral("numeric_raster_note")] = QStringLiteral(
            "The numeric raster of the same name holds the physical values and is intentionally not listed as an Image viewer product; it is a measurement image, not a picture.");
        const bool displaySaved = saveProduct(directory, displayName,
            addProductLegend(color, displayName), label + QStringLiteral(" 路 color display"),
            QStringLiteral("display colors; the paired numeric raster holds the values"), method,
            satellite, norad, raw, camera, epoch, products, projectedOffsetX, projectedOffsetY,
            gcpLatitude, gcpLongitude, gcpPixelX, gcpPixelY, displayMetadata);
        return numericSaved ? (displaySaved ? 2 : 1) : 0;
    };
    int saved = 0;

    const QImage vis = input(QStringLiteral("CH1_VIS.png"));
    if (!vis.isNull()) {
        QJsonObject visFactorMeta;
        visFactorMeta[QStringLiteral("encoding")] = QStringLiteral("uint16; value / 100 = percent reflectance factor; 65535 = no data");
        visFactorMeta[QStringLiteral("no_data_value")] = 65535;
        visFactorMeta[QStringLiteral("projection_center_latitude_deg")] = lat;
        visFactorMeta[QStringLiteral("projection_center_longitude_deg")] = lon;
        visFactorMeta[QStringLiteral("projection")] = QStringLiteral("equirectangular / geographic longitude-latitude grid");
        visFactorMeta[QStringLiteral("solar_zenith_limit_deg")] = 85.0;
        visFactorMeta[QStringLiteral("night_side_mask_applied")] = true;
        visFactorMeta[QStringLiteral("night_side_note")] = QStringLiteral("Pixels with solar zenith angle > 85 degrees are set to no data; this includes night and low-sun twilight. Reflectance factor is not interpreted there.");
        visFactorMeta[QStringLiteral("method_and_limitations")] = QStringLiteral("NSMC FY-2 visible LUT; legacy 8-bit DN reverse-mapped to 6-bit; no atmospheric correction. Regional geolocation is used to mask solar zenith > 85 degrees as no data.");
        const QImage visFactor = nadirstation::products::chinaGeostationaryReflectanceFactor(vis, lat, lon, camera, 1, scan, epoch,
            projectedOffsetX, projectedOffsetY);
        saved += saveProduct(directory, QStringLiteral("Region_VIS_ReflectanceFactor_0p01pct.png"),
            visFactor, QStringLiteral("Regional VIS reflectance factor"), QStringLiteral("0.01 percent reflectance per uint16 pixel"),
            visFactorMeta.value(QStringLiteral("method_and_limitations")).toString(),
            satellite, norad, raw, camera, epoch, products, 0, 0, 0.0, 0.0, 0, 0, visFactorMeta,
            isViewerProduct(QStringLiteral("Region_VIS_ReflectanceFactor_0p01pct.png")));
        const QImage visDisplay = nadirstation::products::visibleReflectanceEnhancement(vis);
        QImage projectedVisDisplay = nadirstation::products::chinaGeostationaryProjection(visDisplay, lat, lon, camera, 1,
            projectedOffsetX, projectedOffsetY, scan);
        if (!visFactor.isNull() && projectedVisDisplay.size() == visFactor.size()) {
            for (int y = 0; y < visFactor.height(); ++y) {
                const auto* factorRow = reinterpret_cast<const quint16*>(visFactor.constScanLine(y));
                auto* displayRow = projectedVisDisplay.scanLine(y);
                for (int x = 0; x < visFactor.width(); ++x)
                    if (factorRow[x] == 65535) displayRow[x] = 115;
            }
        }
        QJsonObject visDisplayMetadata;
        visDisplayMetadata[QStringLiteral("display_note")] = QStringLiteral("Grayscale enhancement of FY-2G sensor reflectance; brighter means higher sensor reflectance. No atmospheric correction. Pixels with solar zenith > 85 degrees are shown as no-data gray.");
        visDisplayMetadata[QStringLiteral("solar_zenith_limit_deg")] = 85.0;
        visDisplayMetadata[QStringLiteral("night_side_mask_applied")] = true;
        visDisplayMetadata[QStringLiteral("night_side_display_color_rgb")] = QStringLiteral("115,115,115");
        visDisplayMetadata[QStringLiteral("display_layout")] = QStringLiteral("map on top; relative-reflectance grayscale legend in appended footer rows");
        QImage visLegend = addProductLegend(projectedVisDisplay, QStringLiteral("Region_VIS_Reflectance_Enhanced.png"));
        if (!visFactor.isNull() && visLegend.format() == QImage::Format_RGB32 && visLegend.width() == visFactor.width()) {
            for (int y = 0; y < visFactor.height() && y < visLegend.height(); ++y) {
                const auto* factorRow = reinterpret_cast<const quint16*>(visFactor.constScanLine(y));
                auto* displayRow = reinterpret_cast<QRgb*>(visLegend.scanLine(y));
                for (int x = 0; x < visFactor.width(); ++x)
                    if (factorRow[x] == 65535) displayRow[x] = qRgb(115, 115, 115);
            }
        }
        saved += saveProduct(directory, QStringLiteral("Region_VIS_Reflectance_Enhanced.png"),
            visLegend,
            QStringLiteral("Regional VIS reflectance enhancement"), QStringLiteral("display grayscale"),
            QStringLiteral("Square-root display stretch of the channel-specific visible reflectance LUT; not surface albedo."),
            satellite, norad, raw, camera, epoch, products, 0, 0, 0.0, 0.0, 0, 0, visDisplayMetadata);
    }

    const QStringList tbbFiles{
        QStringLiteral("CH4_IR1_TBB_0p01K.png"), QStringLiteral("CH5_IR2_TBB_0p01K.png"),
        QStringLiteral("CH3_IR3_TBB_0p01K.png"), QStringLiteral("CH2_IR4_TBB_0p01K.png")};
    int calibratedChannels = 0;
    for (const QString& file : tbbFiles) {
        const QImage tbb = input(file);
        if (tbb.isNull()) continue;
        ++calibratedChannels;
        const QString name = QStringLiteral("Region_") + QFileInfo(file).completeBaseName() + QStringLiteral(".png");
        saved += saveDataProduct(name, tbb,
            QStringLiteral("Regional calibrated brightness temperature ") + QFileInfo(file).completeBaseName(),
            QStringLiteral("0.01 K per uint16 pixel; 65535 is no data"),
            QStringLiteral("Nearest-neighbour geostationary-to-equirectangular sampling of the downlink Calibration-2 LUT product; numeric TBB values are preserved."));
    }
    // A broadband OLR retrieval needs radiative-transfer or collocated flux
    // regression coefficients. With only the IR1 window-channel TBB available,
    // provide an explicitly labeled Stefan-Boltzmann blackbody-equivalent proxy.
    const QImage ir1Source = input(QStringLiteral("CH4_IR1_TBB_0p01K.png"));
    if (!ir1Source.isNull() && ir1Source.format() == QImage::Format_Grayscale16) {
        constexpr double sigma = 5.670374419e-8;
        QImage olrSource(ir1Source.size(), QImage::Format_Grayscale16);
        for (int y = 0; y < ir1Source.height(); ++y) {
            const auto* src = reinterpret_cast<const quint16*>(ir1Source.constScanLine(y));
            auto* dst = reinterpret_cast<quint16*>(olrSource.scanLine(y));
            for (int x = 0; x < ir1Source.width(); ++x) {
                const double kelvin = src[x] / 100.0;
                if (src[x] < 15000 || src[x] > 35000) { dst[x] = 0; continue; }
                dst[x] = static_cast<quint16>(qBound(1, qRound(sigma * std::pow(kelvin, 4) * 10.0), 65535));
            }
        }
        const QString olrName = QStringLiteral("Region_OLR_StefanBoltzmannProxy_0p1Wm2.png");
        QJsonObject olrMeta;
        olrMeta[QStringLiteral("input_channel")] = QStringLiteral("FY-2 IR1 / CH4 calibrated brightness temperature");
        olrMeta[QStringLiteral("encoding")] = QStringLiteral("uint16 grayscale; value / 10 = W m-2; 65535 = no data");
        olrMeta[QStringLiteral("no_data_value")] = 65535;
        olrMeta[QStringLiteral("retrieval_status")] = QStringLiteral("blackbody_equivalent_proxy_not_broadband_OLR");
        olrMeta[QStringLiteral("method_and_limitations")] = QStringLiteral("Stefan-Boltzmann sigma*T^4 applied to IR1 window-channel brightness temperature (emissivity=1). This is a monochromatic-channel brightness-temperature proxy, not top-of-atmosphere broadband outgoing longwave flux; atmospheric absorption, spectral response, emissivity, and regression/radiative-transfer corrections are not modeled.");
        olrMeta[QStringLiteral("source_ir1_height_pixels")] = ir1Source.height();
        QImage olrProjected = nadirstation::products::chinaGeostationaryDataProjection(olrSource, lat, lon, camera, 1,
            projectedOffsetX, projectedOffsetY, scan);
        for (int y = 0; y < olrProjected.height(); ++y) {
            auto* row = reinterpret_cast<quint16*>(olrProjected.scanLine(y));
            for (int x = 0; x < olrProjected.width(); ++x)
                if (row[x] == 0) row[x] = 65535;
        }
        saved += saveProjectedDataProduct(olrName, olrProjected,
            QStringLiteral("OLR 路 Stefan-Boltzmann blackbody-equivalent proxy"),
            QStringLiteral("0.1 W m-2; derived from IR1 TBB, not broadband TOA OLR"),
            olrMeta.value(QStringLiteral("method_and_limitations")).toString(), olrMeta, true);

        const QImage olrMap(directory.filePath(olrName));
        if (!olrMap.isNull() && olrMap.format() == QImage::Format_Grayscale16) {
            QImage color(olrMap.size(), QImage::Format_RGB32);
            const QVector<QColor> stops{QColor(35, 70, 170), QColor(30, 170, 210),
                QColor(65, 190, 110), QColor(245, 210, 55), QColor(225, 75, 45)};
            for (int y = 0; y < olrMap.height(); ++y) {
                const auto* src = reinterpret_cast<const quint16*>(olrMap.constScanLine(y));
                auto* dst = reinterpret_cast<QRgb*>(color.scanLine(y));
                for (int x = 0; x < olrMap.width(); ++x) {
                    const double value = src[x] / 10.0;
                    if (src[x] == 65535) { dst[x] = qRgb(115, 125, 138); continue; }
                    const double t = qBound(0.0, (value - 50.0) / 500.0, 1.0) * (stops.size() - 1);
                    const int i = qMin(static_cast<int>(stops.size()) - 2, static_cast<int>(t));
                    dst[x] = interpolateColor(stops[i], stops[i + 1], t - i).rgb();
                }
            }
            QJsonObject olrDisplayMeta = olrMeta;
            olrDisplayMeta[QStringLiteral("display_of_numeric_product")] = olrName;
            olrDisplayMeta[QStringLiteral("display_layout")] = QStringLiteral("regional map with 50-550 W m-2 comparison color scale in footer");
            olrDisplayMeta[QStringLiteral("map_height_pixels")] = color.height();
            saved += saveProduct(directory, QStringLiteral("Region_OLR_StefanBoltzmannProxy_Enhanced.png"),
                addProductLegend(color, QStringLiteral("Region_OLR_Enhanced")),
                QStringLiteral("OLR 路 blackbody-equivalent proxy (color display)"),
                QStringLiteral("display colors; read numeric values from Region_OLR_StefanBoltzmannProxy_0p1Wm2.png"),
                olrMeta.value(QStringLiteral("method_and_limitations")).toString(),
                satellite, norad, raw, camera, epoch, products, projectedOffsetX, projectedOffsetY,
                gcpLatitude, gcpLongitude, gcpPixelX, gcpPixelY, olrDisplayMeta);
        }

    }
    std::cerr << "[regional] calibrated TBB maps saved; preparing C products\n";
    // Retrieval-like products use all four calibrated infrared channels, on
    // the same projected grid. The thresholds are explicitly marked as local
    // proxies; NSMC's published class codes are used for interoperability.
    const QImage ir1Tbb = input(QStringLiteral("CH4_IR1_TBB_0p01K.png"));
    const QImage ir2Tbb = input(QStringLiteral("CH5_IR2_TBB_0p01K.png"));
    const QImage ir3Tbb = input(QStringLiteral("CH3_IR3_TBB_0p01K.png"));
    const QImage ir4Tbb = input(QStringLiteral("CH2_IR4_TBB_0p01K.png"));
    if (!ir1Tbb.isNull() && !ir2Tbb.isNull() && !ir3Tbb.isNull() && !ir4Tbb.isNull()) {
        const auto projectTbb = [&](const QImage& image) {
            const QImage repaired = nadirstation::products::repairMissingLinesMedian(image, 2);
            // `scan` was missing here before 2026-10-17, so the L2 inputs p1..p4 were placed with
            // the default nominal lattice while the published Region_*_TBB rasters and the
            // projected visible channel used the calibrated geometry: every L2 product mixed
            // two different geolocations. They now all share the per-observation navigation.
            const QImage projected = nadirstation::products::chinaGeostationaryDataProjection(
                repaired, lat, lon, camera, 1, projectedOffsetX, projectedOffsetY, scan);
            return nadirstation::products::repairMissingLinesMedian(projected, 2);
        };
        const QImage p1 = projectTbb(ir1Tbb), p2 = projectTbb(ir2Tbb);
        const QImage p3 = projectTbb(ir3Tbb), p4 = projectTbb(ir4Tbb);
        std::cerr << "[regional] four channels projected\n";

        // Visible reflectance on the same regional grid, for the surface-type decision the
        // cloud classification needs. Table 1.18 distinguishes 晴空海面 from 晴空陆地, and
        // the official auxiliary-parameter list (1.7.3.3) names 下垫面类型数据; without it
        // code 1 is unreachable. The same projected-map shift as p1..p4 is applied so that
        // this array is geolocated identically to them.
        QImage projectedVis;
        if (!vis.isNull()) {
            projectedVis = nadirstation::products::chinaGeostationaryReflectanceFactor(
                vis, lat, lon, camera, 1, scan, epoch, projectedOffsetX, projectedOffsetY);
            std::cerr << "[regional] visible projected for surface type ("
                      << projectedVis.width() << "x" << projectedVis.height() << ")\n";
        }

        // No local-fit OLR retrieval is emitted. The IR1 Stefan-Boltzmann image above is
        // retained only as a clearly named monochromatic blackbody proxy. Published OLR
        // algorithms require their RTM/ancillary inputs or CERES matchup-derived coefficients.
        QImage cloudMask = nadirstation::products::cloudDetectionFromTbb(p1, p2, p3, p4);
        // Daytime pixels: replace the threshold mask with the decision tree trained against the
        // official NSMC cloud classification (daytimeCloudMaskFullDisk; independent-scene agreement
        // 83.3 % vs 78.5 %). Night/twilight pixels keep the threshold mask (the tree did not improve
        // them). The tree runs on the native full-disk grid and is projected like every other input.
        qint64 cloudTreePixels = 0;
        // The tree and the SST coefficients were trained and validated on FY-2G only.
        const bool fy2gTrainedProducts = norad == QStringLiteral("40367");
        if (fy2gTrainedProducts && scan.navigation.valid && !vis.isNull() && cloudMask.format() == QImage::Format_Indexed8) {
            const QImage dayMask = nadirstation::products::daytimeCloudMaskFullDisk(
                ir1Tbb, ir2Tbb, ir3Tbb, ir4Tbb, vis, scan.navigation);
            if (!dayMask.isNull()) {
                const QImage dayGrid = nadirstation::products::chinaGeostationaryDataProjection(
                    dayMask, lat, lon, camera, 1, projectedOffsetX, projectedOffsetY, scan);
                if (dayGrid.size() == cloudMask.size()) {
                    for (int y = 0; y < cloudMask.height(); ++y) {
                        const uchar* d = dayGrid.constScanLine(y);
                        uchar* m = cloudMask.scanLine(y);
                        for (int x = 0; x < cloudMask.width(); ++x)
                            if ((d[x] == 0 || d[x] == 1) && (m[x] == 0 || m[x] == 1)) { m[x] = d[x]; ++cloudTreePixels; }
                    }
                }
            }
        }
        std::cerr << "[regional] CLD daytime decision tree applied to " << cloudTreePixels << " pixels\n";
        qint64 validCloudPixels = 0;
        for (int y = 0; y < cloudMask.height(); ++y) {
            const uchar* row = cloudMask.constScanLine(y);
            for (int x = 0; x < cloudMask.width(); ++x)
                if (row[x] == 0 || row[x] == 1) ++validCloudPixels;
        }
        if (validCloudPixels == 0) {
            // A partial pass can end before any received scan reaches the requested
            // regional grid. Do not leave blank regional rasters in the product list.
            const QStringList regionalFiles = QDir(directory.absolutePath()).entryList(
                QStringList{QStringLiteral("Region_*")}, QDir::Files);
            for (const QString& name : regionalFiles)
                QFile::remove(directory.filePath(name));
            QJsonArray retainedProducts;
            for (const QJsonValue& value : products) {
                if (!value.toObject().value(QStringLiteral("path")).toString()
                         .startsWith(QStringLiteral("Region_")))
                    retainedProducts.append(value);
            }
            products = retainedProducts;
            saved = 0;
            QJsonObject l2Status;
            l2Status[QStringLiteral("status")] = QStringLiteral("withheld_no_valid_regional_pixels");
            l2Status[QStringLiteral("reason")] = QStringLiteral(
                "The received scan lines do not intersect the requested regional grid; all regional output pixels are invalid.");
            l2Status[QStringLiteral("received_ir_lines")] = manifest.value(QStringLiteral("scan_lines_received"));
            l2Status[QStringLiteral("expected_ir_lines")] = manifest.value(QStringLiteral("scan_lines_expected"));
            manifest[QStringLiteral("l2_status")] = l2Status;
            std::cerr << "[regional] L2 withheld: zero valid pixels in requested regional grid; removed blank regional outputs\n";
        } else {
        manifest.remove(QStringLiteral("l2_status"));
        // Convert the indexed mask to a numeric one immediately, before anything
        // can expand its colour table. The index codes (0 clear / 1 cloud /
        // 255 invalid) are the measurement; the colour table exists only so the
        // mask can be displayed.
        //
        // Root cause of the file that held 9546/31861/61678 instead of 0/1/255:
        // saveProjectedDataProduct() calls addProductLegend(), which paints a
        // footer onto an RGB32 canvas and therefore converts its input to RGB32.
        // That conversion applies the palette, so by the time the writer saw the
        // raster its "index" was a grey derived from qRgb(30,38,46) etc. Doing it
        // here keeps the colours for display AND keeps the codes for the file.
        // The mask is handed to the writer in its indexed form so that
        // addProductLegend()'s guard returns it untouched, exactly as it now does
        // for the cloud-class product. Converting it to grayscale here instead
        // sends it down the legend path, which paints through RGB32 and collapses
        // the codes: measured, 0/1/255 came out as 0/0/207. Keeping the indexed
        // form is what makes the codes survive.
        const QImage& cloudMaskNumeric = cloudMask;
        std::cerr << "[regional] CLD mask calculated (format: "
                  << int(cloudMaskNumeric.format()) << " = Indexed8)\n";
        // Count the codes before any colour handling. After the first index fix
        // the saved file held only 0 and 207, with no cloud code at all, and there
        // are two possible reasons: the scene genuinely has no cloud candidates,
        // or the code is lost on the way out. Printing the histogram here
        // separates those instead of leaving it to guesswork.
        {
            qint64 nClear = 0, nCloud = 0, nInvalid = 0, nOther = 0;
            for (int y = 0; y < cloudMaskNumeric.height(); ++y) {
                const uchar* row = cloudMaskNumeric.constScanLine(y);
                for (int x = 0; x < cloudMaskNumeric.width(); ++x) {
                    const int v = row[x];
                    if (v == 0) ++nClear;
                    else if (v == 1) ++nCloud;
                    else if (v == 255) ++nInvalid;
                    else ++nOther;
                }
            }
            std::cerr << "[regional] CLD codes before colour: clear=" << nClear
                      << " cloud=" << nCloud << " invalid=" << nInvalid
                      << " other=" << nOther << "\n";
        }
        const QImage classes = nadirstation::products::cloudClassificationFromTbb(p1, p2, p3, p4,
                                                                        projectedVis);
        std::cerr << "[regional] CLC classes calculated\n";
        QImage ctt = nadirstation::products::cloudTopTemperatureFromTbb(p1, cloudMask);
        // Zero from the helper means clear, rejected, or invalid, never 0 K.
        // Convert that absence code to the sentinel used by the numeric raster.
        for (int y = 0; y < ctt.height(); ++y) {
            auto* row = reinterpret_cast<quint16*>(ctt.scanLine(y));
            for (int x = 0; x < ctt.width(); ++x)
                if (row[x] == 0) row[x] = 65535;
        }
        const QImage cfr = nadirstation::products::cloudFractionFromMask(cloudMask, 2);
        const QImage cfrSliding = nadirstation::products::cloudFractionSlidingFromMask(cloudMask, 3);
        std::cerr << "[regional] CTT/CFR calculated\n";

        // FY-CSR published method (CN111060991A/B): group 3x3 nominal pixels,
        // reject cells whose centre satellite zenith exceeds 60 degrees, derive
        // cloud amount from the L2 mask, and average clear/cloudy BT separately.
        // Here it is applied on the station's 0.05-degree regional grid using
        // this station's cloud mask, so it is a documented regional adaptation,
        // not a byte-for-byte reproduction of the NSMC NOM-grid implementation.
        {
            constexpr double earthRadiusKm = 6378.137;
            constexpr double zenithLimitDeg = 60.0;
            constexpr double pi = 3.14159265358979323846;
            const int cell = 3;
            const int outW = p1.width() / cell;
            const int outH = p1.height() / cell;
            const std::array<const QImage*, 3> channels{&p1, &p2, &p3};
            const std::array<QString, 3> channelNames{QStringLiteral("IR1"), QStringLiteral("IR2"), QStringLiteral("IR3_WV")};
            std::array<QImage, 3> clearBt, cloudBt, allBt;
            QImage amount(QSize(outW, outH), QImage::Format_Grayscale16);
            amount.fill(65535);
            for (int b = 0; b < 3; ++b) {
                clearBt[b] = QImage(QSize(outW, outH), QImage::Format_Grayscale16);
                cloudBt[b] = QImage(QSize(outW, outH), QImage::Format_Grayscale16);
                allBt[b] = QImage(QSize(outW, outH), QImage::Format_Grayscale16);
                clearBt[b].fill(65535);
                cloudBt[b].fill(65535);
                allBt[b].fill(65535);
            }
            const double satX = camera.xKm, satY = camera.yKm, satZ = camera.zKm;
            const double lon0 = lon - 34.0;
            const double lat0 = lat + 21.0;
            const double lonStep = 68.0 / p1.width();
            const double latStep = 42.0 / p1.height();
            qint64 validCells = 0, rejectedByZenith = 0;
            for (int cy = 0; cy < outH; ++cy) {
                auto* amountRow = reinterpret_cast<quint16*>(amount.scanLine(cy));
                std::array<quint16*, 3> clearRows{}, cloudRows{}, allRows{};
                for (int b = 0; b < 3; ++b) {
                    clearRows[b] = reinterpret_cast<quint16*>(clearBt[b].scanLine(cy));
                    cloudRows[b] = reinterpret_cast<quint16*>(cloudBt[b].scanLine(cy));
                    allRows[b] = reinterpret_cast<quint16*>(allBt[b].scanLine(cy));
                }
                for (int cx = 0; cx < outW; ++cx) {
                    const int centerX = cx * cell + 1;
                    const int centerY = cy * cell + 1;
                    const double phi = (lat0 - (centerY + 0.5) * latStep) * pi / 180.0;
                    const double lambda = (lon0 + (centerX + 0.5) * lonStep) * pi / 180.0;
                    const double gx = earthRadiusKm * std::cos(phi) * std::cos(lambda);
                    const double gy = earthRadiusKm * std::cos(phi) * std::sin(lambda);
                    const double gz = earthRadiusKm * std::sin(phi);
                    const double vx = satX - gx, vy = satY - gy, vz = satZ - gz;
                    const double range = std::sqrt(vx * vx + vy * vy + vz * vz);
                    const double cosZenith = ((gx * vx + gy * vy + gz * vz) /
                                              (earthRadiusKm * range));
                    const double zenithDeg = std::acos(std::clamp(cosZenith, -1.0, 1.0)) * 180.0 / pi;
                    if (zenithDeg > zenithLimitDeg) { ++rejectedByZenith; continue; }

                    int clearCount = 0, cloudyCount = 0;
                    std::array<double, 3> clearSum{}, cloudSum{}, allSum{};
                    bool completeCell = true;
                    for (int dy = 0; dy < cell && completeCell; ++dy) {
                        const int y = cy * cell + dy;
                        const auto* maskRow = cloudMask.constScanLine(y);
                        std::array<const quint16*, 3> rows{};
                        for (int b = 0; b < 3; ++b)
                            rows[b] = reinterpret_cast<const quint16*>(channels[b]->constScanLine(y));
                        for (int dx = 0; dx < cell; ++dx) {
                            const int x = cx * cell + dx;
                            const uchar classification = maskRow[x];
                            if (classification != 0 && classification != 1) { completeCell = false; break; }
                            std::array<double, 3> t{};
                            for (int b = 0; b < 3; ++b) {
                                if (rows[b][x] < 15000 || rows[b][x] > 35000) { completeCell = false; break; }
                                t[b] = rows[b][x];
                            }
                            if (!completeCell) break;
                            for (int b = 0; b < 3; ++b) {
                                allSum[b] += t[b];
                                if (classification == 0) clearSum[b] += t[b];
                                else cloudSum[b] += t[b];
                            }
                            if (classification == 0) ++clearCount; else ++cloudyCount;
                        }
                    }
                    if (!completeCell) continue;
                    ++validCells;
                    amountRow[cx] = static_cast<quint16>(qRound((cloudyCount / 9.0) * 10000.0));
                    for (int b = 0; b < 3; ++b) {
                        const double clr = clearCount ? clearSum[b] / clearCount : -1.0;
                        const double cld = cloudyCount ? cloudSum[b] / cloudyCount : -1.0;
                        const double total = allSum[b] / 9.0;
                        if (clr >= 0.0) clearRows[b][cx] = static_cast<quint16>(qRound(clr));
                        if (cld >= 0.0) cloudRows[b][cx] = static_cast<quint16>(qRound(cld));
                        allRows[b][cx] = static_cast<quint16>(qRound(total));
                    }
                }
            }
            QJsonObject csrMeta;
            csrMeta[QStringLiteral("processing_status")]=QStringLiteral("published_FY_CSR_method_regional_grid_adaptation");
            csrMeta[QStringLiteral("preserve_numeric_samples")]=true;
            csrMeta[QStringLiteral("input_channels")]=QJsonArray{QStringLiteral("IR1"),QStringLiteral("IR2"),QStringLiteral("IR3 water-vapour")};
            csrMeta[QStringLiteral("projection_center_latitude_deg")]=lat;
            csrMeta[QStringLiteral("projection_center_longitude_deg")]=lon;
            csrMeta[QStringLiteral("projection")]=QStringLiteral("equirectangular regional grid, 20 pixels per degree before 3x3 aggregation");
            csrMeta[QStringLiteral("source_algorithm")]=QStringLiteral("FY-CSR method disclosed in CN111060991A/B; active claims specify 3x3 grouping, centre-pixel geometry, zenith <=60 deg, L2 cloud-mask fractions, and clear/cloud/all-sky brightness temperatures");
            csrMeta[QStringLiteral("algorithm_reference_url")]=QStringLiteral("https://patents.google.com/patent/CN111060991A/zh");
            csrMeta[QStringLiteral("implementation_deviation")]=QStringLiteral("Applied after regional equirectangular projection at 0.05 degree sampling using the station's own threshold cloud mask; the referenced implementation groups native NOM pixels and uses the NSMC L2 cloud mask. Not an official-equivalent CSR retrieval.");
            csrMeta[QStringLiteral("cell_grouping")]=QStringLiteral("3x3 regional-grid samples; centre sample defines geometry; nominal equatorial cell is about 16.7 km on this 0.05-degree grid");
            csrMeta[QStringLiteral("aggregated_grid_degrees_per_pixel")]=QJsonArray{0.15,0.15};
            csrMeta[QStringLiteral("aggregated_grid_origin_lon_deg")]=lon0+1.5*lonStep;
            csrMeta[QStringLiteral("aggregated_grid_origin_lat_deg")]=lat0-1.5*latStep;
            csrMeta[QStringLiteral("satellite_zenith_limit_deg")]=zenithLimitDeg;
            csrMeta[QStringLiteral("complete_3x3_required")]=true;
            csrMeta[QStringLiteral("valid_complete_cells")]=static_cast<double>(validCells);
            csrMeta[QStringLiteral("rejected_cells_zenith_gt_60_deg")]=static_cast<double>(rejectedByZenith);
            csrMeta[QStringLiteral("mask_coding")]=QStringLiteral("0 clear, 1 cloudy, other invalid");
            csrMeta[QStringLiteral("no_data_value")]=65535;
            csrMeta[QStringLiteral("amount_encoding")]=QStringLiteral("uint16; value / 10000 = cloud fraction 0-1 (0.01 percentage-point units); 65535 = invalid");
            const QString amountLimit=QStringLiteral("Published FY-CSR 3x3 aggregation and radiance-separation method, adapted to the regional grid and local cloud mask. Cloud-mask errors and regional resampling propagate directly; validate against official CSR before quantitative use.");
            saved += saveContinuousProxy(QStringLiteral("Region_CSR_CloudAmount_0p01pct.png"),
                QStringLiteral("Region_CSR_CloudAmount_Enhanced.png"), amount, 0.0, 1.0, 65535,
                QStringLiteral("CSR cloud amount from 3x3 cloud mask"), QStringLiteral("fraction (0-1)"), amountLimit,
                csrMeta, 10000.0, true);
            for (int b=0;b<3;++b) {
                const QString suffix=channelNames[b];
                const QString base="Region_CSR_"+suffix;
                QJsonObject channelMeta=csrMeta;
                channelMeta[QStringLiteral("channel")]=suffix;
                channelMeta[QStringLiteral("temperature_encoding")]=QStringLiteral("uint16; value / 100 = brightness temperature K; 65535 = invalid or class absent");
                const auto emitCsr=[&](const QString& className,const QImage& raster,const QString& description) {
                    QJsonObject meta=channelMeta;
                    meta[QStringLiteral("csr_component")]=className;
                    const QString limit=amountLimit+QStringLiteral(" Component: ")+description;
                    saved+=saveContinuousProxy(base+"_"+className+"_0p01K.png",
                        base+"_"+className+"_Enhanced.png",raster,180.0,320.0,65535,
                        QStringLiteral("CSR ")+suffix+QStringLiteral(" ")+className,
                        QStringLiteral("0.01 K per uint16 pixel"),limit,meta,100.0,true);
                };
                emitCsr(QStringLiteral("ClearSky"),clearBt[b],QStringLiteral("mean BT of clear pixels in each valid 3x3 cell"));
                emitCsr(QStringLiteral("CloudySky"),cloudBt[b],QStringLiteral("mean BT of cloudy pixels in each valid 3x3 cell"));
                emitCsr(QStringLiteral("AllSky"),allBt[b],QStringLiteral("mean BT of all pixels; equals (1-N)*Tclr + N*Tcld when both classes are present"));
            }
            std::cerr << "[regional] published-method CSR generated: cells=" << validCells
                      << " zenith-rejected=" << rejectedByZenith << "\n";
        }

        // PRE and UTH empirical regressions are not emitted as quantitative products.
        // Their published retrievals require first-guess atmospheric fields, radiative
        // transfer and/or independently matched validation data unavailable in this run.
        QJsonObject common;
        common["processing_status"] = QStringLiteral("local_multichannel_cloud_retrieval_proxy");
        common["input_channels"] = QJsonArray{QStringLiteral("IR1"), QStringLiteral("IR2"), QStringLiteral("IR3"), QStringLiteral("IR4")};
        const QJsonObject observationDimensions = manifest.value("dimensions").toObject();
        const int observedIrRasterRows = observationDimensions.value("ir_height").toInt();
        const int reportedReceivedIrLines = manifest.value("scan_lines_received").toInt(-1);
        const int receivedIrLines = reportedReceivedIrLines >= 0
            ? reportedReceivedIrLines : observedIrRasterRows;
        const bool completeDisk = manifest.value("complete_disk").toBool(false);
        common["received_ir_lines"] = receivedIrLines;
        common["received_ir_lines_source"] = reportedReceivedIrLines >= 0
            ? QStringLiteral("FY2_Observation.json:scan_lines_received")
            : QStringLiteral("FY2_Observation.json:dimensions.ir_height fallback");
        common["observed_ir_raster_rows"] = observedIrRasterRows;
        common["complete_disk"] = completeDisk;
        common["expected_full_disk_lines"] = 2291;
        common["incomplete_observation"] = !completeDisk ||
            (receivedIrLines > 0 && receivedIrLines < 2291);
        common["retrieval_caveat"] = QStringLiteral("Uses downlink Calibration-2 brightness temperatures. Only IR1 and IR2 enter the local cloud decision rules; IR3 and IR4 are validity gates only. This does not implement the complete NSMC DPC algorithm, ancillary inputs, or operational quality control.");
        if (norad == QStringLiteral("43491")) {
            QJsonObject reference;
            reference[QStringLiteral("reference_dataset")] = QStringLiteral("NSMC FY2H_FDI_ALL_NOM full-disk HDF, 2026-10-10");
            reference[QStringLiteral("scenes")] = 39;
            reference[QStringLiteral("native_grid")]=QStringLiteral("2288x2288 NOM; station navigation and regional reprojection excluded");
            reference[QStringLiteral("truth_mapping")]=QStringLiteral("NOMCloudClassification: 0/1 clear, 11..15/21 cloud");
            reference[QStringLiteral("valid_pixels")]=117641583;
            reference[QStringLiteral("accuracy")]=0.8213368737;
            reference[QStringLiteral("POD")]=0.8729471515;
            reference[QStringLiteral("false_alarm_ratio")]=0.2251084032;
            reference[QStringLiteral("CSI")]=0.6963556459;
            reference[QStringLiteral("station_cloud_fraction")]=0.5287615519;
            reference[QStringLiteral("official_cloud_fraction")]=0.4693673410;
            reference[QStringLiteral("cloud_fraction_bias")]=0.0593942110;
            reference[QStringLiteral("strict_class_comparison")]=QStringLiteral("Excluding ambiguous official codes 1 and 21: n=57,829,736; accuracy=0.9011; POD=0.99994; FAR=0.1966; CSI=0.8033.");
            reference[QStringLiteral("scope_limit")]=QStringLiteral("Same instrument and official reference products, but one date earlier than the local reception. This validates retrieval behavior on official FY-2H radiances, not the exact received scenes or station navigation.");
            common[QStringLiteral("same_instrument_reference_validation")]=reference;
        }
        QJsonObject cldMeta = common;
        cldMeta["processing_status"] = QStringLiteral("local_ir1_ir2_cloud_decision_proxy");
        // Preserve the mask's categorical index values. addProductLegend() must
        // leave Indexed8 rasters untouched; expanding the palette would turn
        // the codes into display greys and destroy the measurement semantics.
        cldMeta["legend_codes"] = QJsonObject{
            {"0", QStringLiteral("clear / no cloud candidate")},
            {"1", QStringLiteral("cloud candidate")},
            {"255", QStringLiteral("invalid / no data")}};
        cldMeta["encoding"] = QStringLiteral(
            "uint8 index code, preserved verbatim (0=clear, 1=cloud, 255=invalid). "
            "addProductLegend() returns indexed rasters untouched so the codes survive; "
            "before that guard this same file held colour-table greys 9546/31861/61678 "
            "and the cloud code was unrecoverable.");
        cldMeta["preserve_numeric_samples"] = true;
        cldMeta["no_data_value"] = 255;
        cldMeta["decision_channels"] = cloudTreePixels > 0
            ? QJsonArray{QStringLiteral("IR1"), QStringLiteral("IR2"), QStringLiteral("IR3"), QStringLiteral("IR4"), QStringLiteral("VIS")}
            : QJsonArray{QStringLiteral("IR1"), QStringLiteral("IR2")};
        cldMeta["validity_channels"] = QJsonArray{QStringLiteral("IR1"), QStringLiteral("IR2"),
            QStringLiteral("IR3"), QStringLiteral("IR4")};
        cldMeta["channel_role_note"] = QStringLiteral(
            "Night/twilight pixels (and every pixel when the daytime tree is not applied): only IR1 and IR2 enter the clear/cloud decision rules; IR3 and IR4 are checked only for valid range. "
            "Daytime tree pixels (FY-2G only): IR1..IR4 and VIS all enter the decision.");
        cldMeta["daytime_decision_tree_pixels"] = static_cast<double>(cloudTreePixels);
        if (!fy2gTrainedProducts)
            cldMeta["daytime_tree_not_applied"] = QStringLiteral("tree trained on FY-2G only; not validated for this satellite");
        cldMeta["daytime_method"] = QStringLiteral(
            "Solar zenith < 80 deg with the visible channel available: depth-6 decision tree (src/fy2g_cloudtree_day.inc) on native full-disk features "
            "(3x3 IR1..IR4 means and differences, IR1 uniformity and warmest-pixel differences, visible reflectance / cos(sza), land/sea, sun and satellite zenith), "
            "trained against the official NSMC NOMCloudClassification of five hourly FDI files (2026-10-04/05). Agreement with the official product: "
            "leave-one-scene-out 82.1 % (HSS 0.64); independent scene 2026-10-05 07:00 83.3 % (HSS 0.665) vs 78.5 % (HSS 0.571) for the threshold rules. "
            "Night/twilight pixels keep the IR1/IR2 threshold rules (about 77 % agreement).");
        cldMeta["validation_reference"] = QStringLiteral("official NSMC NOMCloudClassification (cloud = codes 11-21, clear = 0/1) mapped through the station navigation; not independent ground truth");
        saved += saveProjectedDataProduct(QStringLiteral("Region_CLD_CloudMask.png"), cloudMaskNumeric,
            QStringLiteral("CLD 路 cloud mask (day: tree trained on NSMC; night: IR1/IR2 rules)"),
            QStringLiteral("uint8 index code preserved verbatim: 0=clear, 1=cloud candidate, "
                           "255=invalid/no data"),
            QStringLiteral("IR1/IR2 decision rules combine cold IR1/IR2, an IR1-IR2 split-window thin-cirrus indication, and local 3x3 IR1 cold contrast. IR3/IR4 only participate in input-validity checks. Not the official operational CLD retrieval."), cldMeta, true, QStringLiteral("Region_CLD_Enhanced.png"));
        std::cerr << "[regional] CLD mask saved with legend\n";
        QJsonObject clcMeta = common;
        clcMeta["processing_status"] = QStringLiteral("local_ir1_ir2_cloud_grouping_candidate");
        clcMeta["legend_codes"] = QJsonObject{
            {"0", QStringLiteral("clear candidate")}, {"1", QStringLiteral("cloud candidate, untyped")},
            {"2", QStringLiteral("cold-cloud candidate")}, {"3", QStringLiteral("warmer-cloud candidate")},
            {"4", QStringLiteral("split-window cirrus candidate")}, {"255", QStringLiteral("invalid / no data")}};
        clcMeta["code_standard_reference"] = QStringLiteral("Project-local candidate grouping only; these codes do not represent NSMC table 1.18 meteorological cloud classes.");
        clcMeta["decision_channels"] = cldMeta.value(QStringLiteral("decision_channels"));
        clcMeta["validity_channels"] = cldMeta.value(QStringLiteral("validity_channels"));
        clcMeta["channel_role_note"] = cldMeta.value(QStringLiteral("channel_role_note"));
        clcMeta["legend_colors"] = QJsonObject{
            {"0", QStringLiteral("#1e262e")}, {"1", QStringLiteral("#e6e6e6")},
            {"2", QStringLiteral("#c85fdc")}, {"3", QStringLiteral("#f5b437")},
            {"4", QStringLiteral("#46cdeb")}, {"255", QStringLiteral("#737d8a")}};
        clcMeta["original_indexed_codes_converted_to_color_display"] = false;
        clcMeta["encoding"] = QStringLiteral(
            "uint8 index code preserved verbatim (0=clear candidate, 1=cloud candidate, "
            "2=cold-cloud candidate, 3=warmer-cloud candidate, 4=split-window cirrus candidate, 255=invalid); a colour "
            "ramp must be applied by the reader, not baked into the file");
        clcMeta["preserve_numeric_samples"] = true;
        clcMeta["no_data_value"] = 255;
        saved += saveProjectedDataProduct(QStringLiteral("Region_CLC_ExperimentalGrouping.png"), classes,
            QStringLiteral("CLC · experimental cloud grouping"),
            QStringLiteral("uint8 project-local candidate code; not an NSMC cloud class"),
            QStringLiteral("Local threshold grouping derived from the station IR1/IR2 cloud mask. IR3/IR4 only gate input validity; they do not affect cloud class. It is not the NSMC FY-2 CLC retrieval or a validated cloud-type classification."), clcMeta, true, QStringLiteral("Region_CLC_Enhanced.png"));
        QJsonObject cttMeta = common;
        cttMeta["temperature_encoding"] = QStringLiteral("uint16 grayscale: brightness temperature in 0.01 K; 65535 means no cloud candidate / invalid");
        cttMeta["encoding"] = QStringLiteral("uint16; brightness temperature K = pixel / 100; 65535 = no cloud candidate / invalid");
        cttMeta["units"] = QStringLiteral("0.01 K per pixel; 65535 = no cloud candidate / invalid");
        cttMeta["no_data_value"] = 65535;
        cttMeta["physical_interpretation"] = QStringLiteral("Cloud-candidate IR1 brightness-temperature diagnostic. It assumes opaque cloud for a possible cloud-top interpretation; it is not cloud-top height and is not corrected with NWP profiles or emissivity.");
        cttMeta["reference_validation"] = QStringLiteral("IR1 TBB collocation diagnostic only: the selected station IR1 brightness temperatures are compared against official TBB_IR1_NOM on four 2026-10-04 scenes. Estimated scan-midpoint gaps are 8.86–16.50 min; r=0.3326, bias=-7.10768 K, MAE=22.48398 K, RMSE=29.16448 K over 2,638,315 cells. These are not CTT retrieval accuracy metrics; no official CTT reference raster was available.");
        cttMeta["reference_validation_quantity"] = QStringLiteral("selected station IR1 brightness temperature vs official IR1 TBB; not CTT skill");
        cttMeta["reference_validation_pair_time_reference"] = QStringLiteral("estimated scan midpoint from folder timestamp and manifest created_utc; per-pixel scan timing is not represented");
        cttMeta["ctt_accuracy_validated"] = false;
        cttMeta["reference_validation_scene_count"] = 4;
        cttMeta["reference_validation_common_cells"] = 2638315;
        cttMeta["reference_validation_r"] = 0.3326;
        cttMeta["reference_validation_bias_K"] = -7.10768;
        cttMeta["reference_validation_mae_K"] = 22.48398;
        cttMeta["reference_validation_rmse_K"] = 29.16448;
        saved += saveProjectedDataProduct(QStringLiteral("Region_CTT_IR1_TbbCandidate_0p01K.png"), ctt,
            QStringLiteral("CTT 路 IR1 cloud-top temperature candidate (not height)"), QStringLiteral("0.01 K per pixel; 65535 outside cloud candidates / invalid"),
            QStringLiteral("Copies calibrated IR1 TBB only where the IR1/IR2 decision rules mark a cloud candidate and all four IR channels pass validity checks; cloud-top interpretation assumes opaque cloud and should be treated as provisional."), cttMeta);
        const QImage cttForDisplay = nadirstation::products::repairMissingLinesMedian(ctt, 2);
        QImage cttColor(cttForDisplay.size(), QImage::Format_RGB32);
        const QVector<QColor> cttStops{QColor(45, 80, 220), QColor(45, 205, 235),
            QColor(55, 190, 105), QColor(245, 220, 55), QColor(245, 145, 45), QColor(220, 50, 55)};
        for (int y = 0; y < cttForDisplay.height(); ++y) {
            const auto* src = reinterpret_cast<const quint16*>(cttForDisplay.constScanLine(y));
            auto* dst = reinterpret_cast<QRgb*>(cttColor.scanLine(y));
            for (int x = 0; x < cttForDisplay.width(); ++x) {
                const int value = src[x];
                if (value < 15000 || value > 35000) {
                    dst[x] = qRgb(115, 125, 138);
                    continue;
                }
                const double temperatureK = value / 100.0;
                const double t = qBound(0.0, (temperatureK - 180.0) / 140.0, 1.0) * (cttStops.size() - 1);
                const int i = qMin(static_cast<int>(cttStops.size()) - 2, static_cast<int>(t));
                dst[x] = interpolateColor(cttStops[i], cttStops[i + 1], t - i).rgb();
            }
        }
        QJsonObject cttDisplayMeta = cttMeta;
        cttDisplayMeta["display_of_numeric_product"] = QStringLiteral("Region_CTT_IR1_TbbCandidate_0p01K.png");
        cttDisplayMeta["legend"] = QStringLiteral("65535/no data=gray; 180 K=blue; 220 K=cyan; 260 K=green; 300 K=orange; 320 K=red; colors interpolate continuously");
        cttDisplayMeta["display_layout"] = QStringLiteral("temperature-colored map on top; 180-320 K legend in appended footer rows");
        saved += saveProduct(directory, QStringLiteral("Region_CTT_IR1_TbbCandidate_Enhanced.png"),
            addProductLegend(cttColor, QStringLiteral("Region_CTT_Enhanced")),
            QStringLiteral("CTT 路 IR1 cloud-top temperature candidate (color display)"),
            QStringLiteral("color maps continuously from 180 K blue to 320 K red; gray means no candidate / no data"),
            QStringLiteral("Display copy of the numeric CTT candidate raster; this remains a brightness-temperature candidate, not a cloud-height measurement."),
            satellite, norad, raw, camera, epoch, products, projectedOffsetX, projectedOffsetY,
            gcpLatitude, gcpLongitude, gcpPixelX, gcpPixelY, cttDisplayMeta);
        QJsonObject cfrMeta = common;
        cfrMeta["grid_cell"] = QStringLiteral("2x2 projected pixels; approximately 11 km at equator on the current 20-pixels/degree regional grid; east-west distance decreases with latitude");
        cfrMeta["encoding"] = QStringLiteral("grayscale value 0,25,50,75,100 = cloudy-pixel count / 4 in a complete 2x2 cell; 255 = cell contains invalid/missing samples");
        cfrMeta["cta1_definition"] = QStringLiteral("Ac=N/4 for each 2x2 pixel unit; units with any invalid input sample are no-data, not treated as clear");
        cfrMeta["algorithm_reference"] = QStringLiteral("Du et al. (2024), Operational application of Fengyun geostationary meteorological satellites to cloud observation products, Scientific Reports, section Cloud total amount methods");
        cfrMeta["algorithm_reference_url"] = QStringLiteral("https://pmc.ncbi.nlm.nih.gov/articles/PMC11297144/");
        cfrMeta["implementation_limitations"] = QStringLiteral("Published CTA1 aggregation applied to the station's candidate cloud mask on its regional grid; this is not an NSMC NOM-grid operational retrieval, and no claim is made about the candidate-mask accuracy beyond the separate reported validation.");
        cfrMeta["preserve_numeric_samples"] = true;
        cfrMeta["no_data_value"] = 255;
        saved += saveProjectedDataProduct(QStringLiteral("Region_CFR_2x2_CloudFractionPercent.png"), cfr,
            QStringLiteral("CTA1 路 legacy 2x2 cloud amount"), QStringLiteral("percent cloudy candidates per complete 2x2 projected-pixel cell"),
            QStringLiteral("Legacy-named PNG carrying a published CTA1-style 2x2 regional cloud amount from the station's candidate mask. It is neither the published FY-2G 3x3 sliding CFR nor the NSMC NOM-grid retrieval."), cfrMeta, true, QStringLiteral("Region_CFR_Enhanced.png"));
        QJsonObject cfr3Meta = common;
        cfr3Meta[QStringLiteral("grid_cell")] = QStringLiteral("3x3 sliding window on the regional 0.05-degree grid; each output sample is centered on one input pixel");
        cfr3Meta[QStringLiteral("encoding")] = QStringLiteral("grayscale 0-100 = cloudy pixels / 9; 255 = no data");
        cfr3Meta[QStringLiteral("algorithm_reference")] = QStringLiteral("Liu, Cui & Xiao (2017), The Bias Analysis of FY-2G Cloud Fraction in Summer and Winter, section 1.2");
        cfr3Meta[QStringLiteral("algorithm_reference_url")] = QStringLiteral("https://html.rhhz.net/yyqxxb/html/20170205.htm");
        cfr3Meta[QStringLiteral("implementation_limitations")] = QStringLiteral("Implements the published FY-2G CFR 3x3 sliding cloudy-pixel fraction on the regional grid. Input is this station's provisional cloud-candidate mask, not the NSMC operational cloud mask; windows touching invalid pixels and the outer one-pixel border are no-data.");
        cfr3Meta[QStringLiteral("preserve_numeric_samples")] = true;
        cfr3Meta[QStringLiteral("no_data_value")] = 255;
        saved += saveProjectedDataProduct(QStringLiteral("Region_CFR_3x3Sliding_CloudFractionPercent.png"), cfrSliding,
            QStringLiteral("CFR · 3x3 sliding cloud fraction"), QStringLiteral("percent cloudy candidates in each 3x3 sliding window"),
            QStringLiteral("Published FY-2G CFR window definition applied to the station's provisional cloud-candidate mask on its regional grid."), cfr3Meta, true, QStringLiteral("Region_CFR_3x3Sliding_Enhanced.png"));
        // No empirically fitted CTA correction is emitted. The published FY-CSR
        // cloud amount is generated above directly from its 3x3 cloud-mask definition.

        // Cloud-top height and low-cloud / fog products derived from the
        // calibrated brightness temperatures. Both invert a standard-atmosphere
        // temperature profile, so they are documented as climatological
        // retrievals rather than a substitute for a radiosonde-matched product.
        {
            const QImage cth = nadirstation::products::cloudTopHeightFromTbb(p1, 273.15);
            QJsonObject cthMeta = common;
            cthMeta["processing_status"] = QStringLiteral("standard_atmosphere_temperature_inversion");
            cthMeta["encoding"] = QStringLiteral("uint16; value = metres above mean sea level; 65535 = clear sky / no data");
            cthMeta["no_data_value"] = 65535;
            cthMeta["atmosphere"] = QStringLiteral("ICAO standard atmosphere: 288.15 K at 1013.25 hPa, 6.5 K/km to 11 km, then isothermal to a 1.0 K/km warming stratosphere");
            cthMeta["method_and_limitations"] = QStringLiteral("Cloud-top height inverted from the IR1 brightness temperature through a standard-atmosphere profile. Real temperature soundings differ from the standard atmosphere by several kelvin, which shifts the retrieved height by hundreds of metres; thin or semi-transparent cloud makes the retrieved top too low. Use as a climatological candidate field only.");
            saved += saveContinuousProxy(QStringLiteral("Region_CTH_IR1_HeightMetres.png"),
                QStringLiteral("Region_CTH_Enhanced.png"), cth, 0, 20000, 65535,
                QStringLiteral("CTH 路 IR1 cloud-top height candidate"), QStringLiteral("metres"),
                cthMeta.value(QStringLiteral("method_and_limitations")).toString(), cthMeta, 1.0, true);

            const QImage fog = nadirstation::products::fogLowCloudFromTbb(p1, p2);
            QJsonObject fogMeta = common;
            fogMeta["processing_status"] = QStringLiteral("split_window_low_cloud_candidate");
            fogMeta["input_channels"] = QJsonArray{QStringLiteral("IR1"), QStringLiteral("IR2")};
            fogMeta["encoding"] = QStringLiteral("uint16; value 0-100 = detection confidence percent; 65535 = not applicable / no data");
            fogMeta["no_data_value"] = 65535;
            fogMeta["method"] = QStringLiteral("Warm cloud top (265-295 K) combined with a small IR1-IR2 split-window difference (-1 to 4 K); high optically thin cloud is rejected by the warm-top requirement.");
            fogMeta["method_and_limitations"] = QStringLiteral("Low-stratus and fog candidate from the brightness-temperature split window alone. No visible-channel texture, solar-zenith or surface-elevation test is applied, so warm low cloud over cold high terrain and thin mid-level cloud can be misclassified. The confidence value is a heuristic, not a calibrated probability.");
            saved += saveContinuousProxy(QStringLiteral("Region_FOG_LowCloudConfidence.png"),
                QStringLiteral("Region_FOG_Enhanced.png"), fog, 0, 100, 65535,
                QStringLiteral("FOG 路 low-stratus / fog candidate confidence"), QStringLiteral("percent"),
                fogMeta.value(QStringLiteral("method_and_limitations")).toString(), fogMeta, 1.0, true,
                QVector<QColor>{QColor(25, 45, 80), QColor(255, 70, 170)});

            const QImage lst = nadirstation::products::landSurfaceTemperatureProxy(p1, p2, vis);
            QJsonObject lstMeta = common;
            lstMeta["processing_status"] = QStringLiteral("split_window_clear_sky_land_candidate");
            lstMeta["input_channels"] = QJsonArray{QStringLiteral("IR1"), QStringLiteral("IR2"), QStringLiteral("VIS")};
            lstMeta["encoding"] = QStringLiteral("uint16; value = kelvin x 100; 65535 = not applicable / no data");
            lstMeta["no_data_value"] = 65535;
            lstMeta["method"] = QStringLiteral("LST = T_IR1 + 1.1 x (T_IR1 - T_IR2) + 0.6 K, kept only where the 11-12 um difference is small and the visible channel indicates a bright (land-like) surface.");
            lstMeta["method_and_limitations"] = QStringLiteral("Two-channel split-window land surface temperature candidate. No land mask, no surface emissivity, no atmospheric correction and no diurnal model, so dark-water pixels are excluded by a visible-reflectance test rather than by geography and bright desert can still be confused with thin cloud.");
            saved += saveContinuousProxy(QStringLiteral("Region_LST_ClearSkyProxy_0p01K.png"),
                QStringLiteral("Region_LST_Enhanced.png"), lst, 260, 340, 65535,
                QStringLiteral("LST 路 clear-sky split-window candidate"), QStringLiteral("0.01 K"),
                lstMeta.value(QStringLiteral("method_and_limitations")).toString(), lstMeta, 100.0, true);
        }

        // ---------------------------------------------------------------------------------
        // SST: MCSST on the full-disk grid (seaSurfaceTemperatureFullDisk), validated against
        // NOAA iQuam quality-level-5 buoys. Daytime pixels are visible-screened; night/twilight
        // candidates are published only where they agree (>= reference - 1 K) with daytime
        // retrievals of this station from the previous 20 hours (Region_SST_MCSST_* products of
        // sibling observation folders), because infrared-only screening lets low cloud through.
        // ---------------------------------------------------------------------------------
        const QImage landMask = landMaskForRegion(lat, lon, p1.size());
        if (landMask.isNull()) {
            std::cerr << "[regional] Natural Earth land mask unavailable; skipped SST\n";
        } else if (!scan.navigation.valid) {
            std::cerr << "[regional] SST withheld: observation has no per-observation navigation\n";
        } else if (!fy2gTrainedProducts) {
            std::cerr << "[regional] SST withheld: MCSST coefficients are fitted/validated for FY-2G only\n";
        } else {
            const nadirstation::products::SstRetrieval sr =
                nadirstation::products::seaSurfaceTemperatureFullDisk(ir1Tbb, ir2Tbb, vis, scan.navigation);
            const QImage sstGrid = nadirstation::products::chinaGeostationaryDataProjection(
                sr.sst01K, lat, lon, camera, 1, projectedOffsetX, projectedOffsetY, scan);
            const QImage flagGrid = nadirstation::products::chinaGeostationaryDataProjection(
                sr.flags, lat, lon, camera, 1, projectedOffsetX, projectedOffsetY, scan);
            // Reference field: daytime SST of this station from sibling folders, previous 20 h.
            const QDateTime thisStart = scan.navigation.scanStartUtc;
            QVector<QImage> refSst, refFlag;
            QJsonArray referenceScenes;
            QDir parent(directory.absolutePath()); parent.cdUp();
            for (const QString& sibling : parent.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
                const QDir other(parent.filePath(sibling));
                if (other.absolutePath() == directory.absolutePath()) continue;
                const QString sstPath = other.filePath(QStringLiteral("Region_SST_MCSST_0p01K.png"));
                const QString flagPath = other.filePath(QStringLiteral("Region_SST_MCSST_QualityFlags.png"));
                QFile sideFile(sstPath + QStringLiteral(".json"));
                if (!QFile::exists(sstPath) || !QFile::exists(flagPath) || !sideFile.open(QIODevice::ReadOnly)) continue;
                const QJsonObject side = QJsonDocument::fromJson(sideFile.readAll()).object();
                const QDateTime otherStart = QDateTime::fromString(side.value(QStringLiteral("scan_start_utc")).toString(), Qt::ISODate);
                if (!otherStart.isValid() || otherStart >= thisStart || otherStart.secsTo(thisStart) > 20 * 3600) continue;
                if (side.value(QStringLiteral("projection_center_latitude_deg")).toDouble(-999) != lat ||
                    side.value(QStringLiteral("projection_center_longitude_deg")).toDouble(-999) != lon) continue;
                const QImage os = QImage(sstPath).convertToFormat(QImage::Format_Grayscale16);
                const QImage of = QImage(flagPath).convertToFormat(QImage::Format_Grayscale8);
                if (os.size() != sstGrid.size() || of.size() != sstGrid.size()) continue;
                refSst.push_back(os); refFlag.push_back(of);
                referenceScenes.append(sibling);
            }
            QImage sst(sstGrid.size(), QImage::Format_Grayscale16);
            sst.fill(65535);
            QImage quality(sstGrid.size(), QImage::Format_Grayscale8);
            quality.fill(0);
            qint64 dayKept = 0, nightKept = 0, nightNoReference = 0, nightRejected = 0;
            std::vector<int> refValues;
            for (int y = 0; y < sstGrid.height(); ++y) {
                const auto* sv = reinterpret_cast<const quint16*>(sstGrid.constScanLine(y));
                const auto* fv = flagGrid.constScanLine(y);
                const auto* land = landMask.constScanLine(y);
                auto* outRow = reinterpret_cast<quint16*>(sst.scanLine(y));
                auto* qRow = quality.scanLine(y);
                for (int x = 0; x < sstGrid.width(); ++x) {
                    if (land[x] || sv[x] == 65535 || sv[x] == 0 || fv[x] == 0) continue;
                    if (fv[x] == 1) { outRow[x] = sv[x]; qRow[x] = 1; ++dayKept; continue; }
                    refValues.clear();
                    for (int k = 0; k < refSst.size(); ++k)
                        for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx) {
                            const int yy = y + dy, xx = x + dx;
                            if (yy < 0 || xx < 0 || yy >= sstGrid.height() || xx >= sstGrid.width()) continue;
                            if (refFlag[k].constScanLine(yy)[xx] != 1) continue;
                            const quint16 v = reinterpret_cast<const quint16*>(refSst[k].constScanLine(yy))[xx];
                            if (v != 65535 && v != 0) refValues.push_back(v);
                        }
                    if (refValues.empty()) { ++nightNoReference; continue; }
                    std::nth_element(refValues.begin(), refValues.begin() + refValues.size() / 2, refValues.end());
                    if (static_cast<int>(sv[x]) < refValues[refValues.size() / 2] - 100) { ++nightRejected; continue; }
                    outRow[x] = sv[x]; qRow[x] = 2; ++nightKept;
                }
            }
            QJsonObject sstMeta;
            sstMeta[QStringLiteral("processing_status")] = QStringLiteral("mcsst_buoy_validated_station_retrieval");
            sstMeta[QStringLiteral("algorithm")] = QStringLiteral("SST[K] = 3.47526 + 0.966693*T11 + 3.18478*(T11-T12) + 0.0793334*(T11-T12)*(sec(satz)-1); T11/T12 = 3x3 means on the native full-disk grid");
            sstMeta[QStringLiteral("cloud_screening")] = QStringLiteral("satellite-only, thresholds fixed a priori: T11>=271.15 K; T11 within 3 K of the 11x11 warmest; 5x5 T11 SD<=0.6 K (day) / 0.4 K (night); -0.5<=T11-T12<=5 K; satellite zenith<=60 deg; day (sza<80): VIS/cos(sza)<8 % and 3x3 VIS SD<1 %; night/twilight: must be >= (median of this station's daytime SST within 5x5 cells over the previous 20 h) - 1 K, otherwise withheld");
            sstMeta[QStringLiteral("validation_reference")] = QStringLiteral("NOAA iQuam v2.10 quality_level 5 drifting/moored buoys (October 2026 file), |dt|<=30 min against the per-pixel scan time, 19 FY-2G scenes 2026-10-04..06");
            sstMeta[QStringLiteral("validation_day_leave_one_day_out")] = QStringLiteral("n=357, bias +0.06 K, RMSE 0.81 K, 78 % within 1 K");
            sstMeta[QStringLiteral("validation_night_with_reference_test")] = QStringLiteral("n=26, bias +0.01 K, RMSE 0.69 K (small sample; same coefficients)");
            sstMeta[QStringLiteral("validation_previous_proxy_same_matchups")] = QStringLiteral("T11+1.5*(T11-T12): bias -1.53 K, RMSE 3.00 K");
            sstMeta[QStringLiteral("validation_limits")] = QStringLiteral("three days, one spacecraft (FY-2G), October tropical/subtropical West Pacific and Indian Ocean; coefficients should be refitted as matchups accumulate. Not validated for FY-2H.");
            sstMeta[QStringLiteral("encoding")] = QStringLiteral("uint16; value / 100 = kelvin; 65535 = land, cloud, outside limits or not verifiable");
            sstMeta[QStringLiteral("no_data_value")] = 65535;
            sstMeta[QStringLiteral("preserve_numeric_samples")] = true;
            sstMeta[QStringLiteral("scan_start_utc")] = scan.navigation.scanStartUtc.toString(Qt::ISODate);
            sstMeta[QStringLiteral("scan_end_utc")] = scan.navigation.scanEndUtc.toString(Qt::ISODate);
            sstMeta[QStringLiteral("projection_center_latitude_deg")] = lat;
            sstMeta[QStringLiteral("projection_center_longitude_deg")] = lon;
            sstMeta[QStringLiteral("day_pixels")] = static_cast<double>(dayKept);
            sstMeta[QStringLiteral("night_pixels_published")] = static_cast<double>(nightKept);
            sstMeta[QStringLiteral("night_pixels_rejected_by_reference")] = static_cast<double>(nightRejected);
            sstMeta[QStringLiteral("night_pixels_withheld_no_reference")] = static_cast<double>(nightNoReference);
            sstMeta[QStringLiteral("reference_scenes")] = referenceScenes;
            sstMeta[QStringLiteral("land_mask_source")] = QStringLiteral("bundled Natural Earth 1:110m country polygons");
            sstMeta[QStringLiteral("method_and_limitations")] = QStringLiteral(
                "Station MCSST retrieval with buoy-fitted coefficients and satellite-only cloud screening; see the algorithm, cloud_screening and validation fields. "
                "Bulk (buoy) SST, not skin SST. Night pixels appear only where a daytime retrieval from the previous 20 h exists nearby. "
                "Not the NSMC operational SST (no NWP or 30-day composite); accuracy outside the validation period and region is unverified.");
            saved += saveContinuousProxy(QStringLiteral("Region_SST_MCSST_0p01K.png"),
                QStringLiteral("Region_SST_MCSST_Enhanced.png"), sst, 285.0, 305.0, 65535,
                QStringLiteral("SST - MCSST (buoy-validated)"), QStringLiteral("0.01 K"),
                sstMeta.value(QStringLiteral("method_and_limitations")).toString(), sstMeta, 100.0, true);
            QJsonObject qMeta;
            qMeta[QStringLiteral("encoding")] = QStringLiteral("uint8; 0 = no SST, 1 = daytime (visible-screened), 2 = night/twilight passed the 20-h daytime reference test");
            qMeta[QStringLiteral("no_data_value")] = 0;
            qMeta[QStringLiteral("scan_start_utc")] = scan.navigation.scanStartUtc.toString(Qt::ISODate);
            qMeta[QStringLiteral("preserve_numeric_samples")] = true;
            saved += saveProduct(directory, QStringLiteral("Region_SST_MCSST_QualityFlags.png"), quality,
                QStringLiteral("SST quality flags"), QStringLiteral("flag"), qMeta.value(QStringLiteral("encoding")).toString(),
                satellite, norad, raw, camera, epoch, products, projectedOffsetX, projectedOffsetY,
                gcpLatitude, gcpLongitude, gcpPixelX, gcpPixelY, qMeta, false) ? 1 : 0;
            std::cerr << "[regional] SST MCSST: day " << dayKept << ", night published " << nightKept
                      << ", night rejected " << nightRejected << ", night without reference " << nightNoReference
                      << " (" << refSst.size() << " reference scenes)\n";
        }
        }
    }

    manifest["products"] = products;
    QString productGuide = QStringLiteral(R"FY2(# FY-2G 区域产品说明

本目录中展示图用于快速浏览；定量结果以同名数值图和 JSON 元数据为准。业务算法等价性尚未建立的产品均明确标为候选或实验产品。

| 产品 | 含义与限制 |
|---|---|
| VIS Enhanced | 可见光通道显示值；没有大气校正，不直接等于地表反射率。 |
| TBB | 红外通道亮温，数值编码为 0.01 K；定标表一致不代表反演物理量准确。 |
| CLD / CLC | 云掩膜：白天用以 NSMC 官方云分类训练的决策树（独立时次一致率 83%），夜间用 IR1/IR2 阈值（约 77%）。CLC 仍为本站分组候选，不等同于 NSMC 云分类。 |
| CFR | 按已发表 FY-2 系列 CTA1 形式统计 2×2 单元内云像素数/4；仅输出四像元都有效的单元，其余为 no-data。输入仍是本站候选云掩膜，且使用区域网格，不等同于 NSMC NOM 业务 CTA。 |
| CTT | 云候选像素的红外亮温，不是经大气廓线校正的云顶温度。 |
| CSR | FY-CSR 公布的 3×3 聚合方法区域适配；使用本站云掩膜和区域网格，属于实验产品。 |
| OLR proxy | 单通道亮温对应的黑体等效窄带通量代理，不是宽波段 TOA OLR。 |
| SST MCSST | 本站 MCSST 反演（浮标拟合系数、仅用卫星数据的云检测）。iQuam QL5 浮标按天留一验证：白天偏差 +0.06 K、RMSE 0.81 K；夜间需通过前 20 小时白天参考场检验（n=26，RMSE 0.69 K）。仅 3 天、FY-2G 验证。 |
| LST proxy | 晴空地表双窗候选；缺少完整发射率与大气订正，不是业务 LST。 |

不得把单日拟合、空间留块结果或与单个官方时次的相关性称为跨日业务精度。
)FY2");
    productGuide.replace(QStringLiteral("FY-2G"), satellite);
    QFile::remove(directory.filePath(QStringLiteral("鍖哄煙浜у搧璇存槑.md")));
    QFile guideFile(directory.filePath(QStringLiteral("区域产品说明.md")));
    if (guideFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        guideFile.write(productGuide.toUtf8());
        guideFile.close();
    }
    QJsonObject regionalProjection;
    regionalProjection["center_latitude_deg"] = lat;
    regionalProjection["center_longitude_deg"] = lon;
    regionalProjection["grid"] = QStringLiteral("equirectangular / geographic longitude-latitude");
    regionalProjection["longitude_span_deg"] = 68.0;
    regionalProjection["latitude_span_deg"] = 42.0;
    regionalProjection["satellite"] = satellite;
    regionalProjection["norad_id"] = norad;
    manifest["regional_projection"] = regionalProjection;
    if (projectedOffsetX != 0 || projectedOffsetY != 0) {
        QJsonObject gcp;
        gcp["type"] = QStringLiteral("approximate_single_point_translation");
        gcp["latitude_deg"] = gcpLatitude;
        gcp["longitude_deg"] = gcpLongitude;
        gcp["observed_source_x"] = gcpPixelX;
        gcp["observed_source_y"] = gcpPixelY;
        gcp["projected_translation_x_pixels"] = projectedOffsetX;
        gcp["projected_translation_y_pixels"] = projectedOffsetY;
        gcp["validated_observation_only"] = directory.dirName();
        manifest["navigation_gcp"] = gcp;
    }
    const QString observationFileName = QFileInfo(observationPath).fileName();
    const bool preserveLegacyFy2Name = observationFileName == QStringLiteral("FY2G_Observation.json") ||
                                       observationFileName == QStringLiteral("FY2H_Observation.json");
    manifestFile.setFileName(preserveLegacyFy2Name
        ? observationPath : directory.filePath(QStringLiteral("FY2_Observation.json")));
    if (manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        manifestFile.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented));
        manifestFile.close();
        QFile::remove(directory.filePath(QStringLiteral("MGAS_Observation.json")));
    }
    std::cout << "Saved " << saved << " regional product images for " << satellite.toLocal8Bit().constData()
              << " at " << epoch.toUTC().toString(Qt::ISODateWithMs).toLocal8Bit().constData()
              << "; TLE subpoint " << raw.subpointLongitudeDeg << " E, " << raw.subpointLatitudeDeg
              << " N; scan axis " << camera.subpointLongitudeDeg << " E, " << camera.subpointLatitudeDeg
              << " N; applied projected-map shift " << projectedOffsetX << ", " << projectedOffsetY << " px.\n";
    const bool l2Withheld = manifest.value(QStringLiteral("l2_status")).isObject();
    if (calibratedChannels > 0 && l2Withheld)
        std::cout << "Regional product generation completed; L2 was withheld because the selected region contains no valid pixels.\n";
    else if (calibratedChannels > 0)
    std::cout << "Generated calibrated TBB maps, regional CSR method-adaptation rasters, OLR proxy, SST candidate, and four-channel CLD/CLC/CTT/CFR products; see sidecars for limitations.\n";
    else
        std::cout << "No downlink Calibration-2 TBB channels are present; physical TBB and TBB-derived L2-style maps were skipped. Other multi-channel/time-series retrievals require additional calibration or observations.\n";
    return (saved > 0 || l2Withheld) ? 0 : 6;
}

