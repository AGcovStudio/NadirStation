// An MCIR composite that colours the cloud layer by type instead of drawing it in grey.
//
// The existing chinaGeostationaryMcirProjection() does what the MCIR description asks for --
// it strips the cloud from the image and lays it over a geographic map, white and progressively
// less transparent as the cloud thickens -- but it draws every cloud the same neutral grey. The
// description's stated purpose is to "help differentiate some cloud types from land in single-band
// infrared imagery", and a single grey layer cannot do that: a thin cirrus shield and a
// cumulonimbus anvil come out looking alike, which is exactly the confusion the product exists to
// remove.
//
// This version keeps the basemap and the alpha model and adds two channels of discrimination,
// neither of which is available to a single-band enhancement:
//
//     IR1 brightness temperature   the anchor. Cloud is present where the surface is obscured, and
//                                  colder means higher and thicker, so it drives both opacity and
//                                  whiteness.
//     IR1 - IR2 split window       separates thick deep cloud from thin cirrus. Both are cold in
//                                  IR1, but a thick cloud has a larger split-window difference
//                                  because the two channels see different depths into it.
//     IR3 water vapour absorption  marks high cloud, which is what a 6.7 um channel responds to.
//     visible reflectance          where the sun is up, provides texture: thick low cloud is bright
//                                  and optically rough, thin cirrus is smooth.
//
// The colours are chosen so the result stays readable as a weather image rather than becoming a
// false-colour curiosity: deep convection goes warm and saturated, cirrus stays pale and cool,
// low cloud stays neutral, and the basemap keeps its own colours underneath.

#pragma once

#include "imageproducts.h"

namespace nadirstation::products {

// Coloured MCIR. `ir2` and `ir3` may be null, in which case the corresponding discrimination is
// skipped and the cloud falls back to the neutral white of the plain MCIR. `visible` may be null,
// which is the normal case at night.
//
// All inputs are full-disk channel images on the same grid as `ir1`. Brightness temperatures are
// expected in hundredths of a kelvin when `sourceIsBrightnessTemperature` is set, matching the TBB
// products this project writes.
QImage chinaGeostationaryMcirColour(const QImage& ir1,
                                    const QImage& ir2,
                                    const QImage& ir3,
                                    const QImage& visible,
                                    double centerLatitudeDeg,
                                    double centerLongitudeDeg,
                                    const nadirstation::orbit::SatellitePosition& satellite,
                                    bool sourceIsBrightnessTemperature,
                                    int sourceFirstLine,
                                    int projectedLayerOffsetX,
                                    int projectedLayerOffsetY);

}  // namespace nadirstation::products
