# Third-party notices

NADIR Station is licensed under the GNU Affero General Public License v3.0 (see `LICENSE`).
The components and assets below are not original to this project. Each keeps its own license,
and each of those licenses is compatible with distribution under AGPL-3.0.

## Bundled source code (`third_party/`)

| Component | Version | License | Use |
|---|---|---|---|
| [dnwrnr/sgp4](https://github.com/dnwrnr/sgp4) | — | Apache-2.0 | Orbit propagation (passes, ground track, Doppler) |
| [OpenJPEG](https://github.com/uclouvain/openjpeg) | 2.5.3 | BSD-2-Clause | JPEG 2000 decoding of GK-2A HRIT images |
| [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo) | 3.1.2 | IJG / BSD-3-Clause / zlib | 10/12-bit JPEG decoding of HRIT images |
| EUMETSAT PublicDecompWT (via the [pyPublicDecompWT](https://github.com/sbrodehl/pyPublicDecompWT) mirror) | 2.8.1 | Apache-2.0 | Wavelet / JPEG / T.4 decompression of ELEKTRO-L xRIT |

The original license files are kept in each folder. For PublicDecompWT only the COMP decompressors
and `DISE/ErrorHandling` are built. `DISE/Types.h` is modified (the `PUBLICDECOMPWT_NO_TIME`
switch), as its README in this repository notes.

## Libraries linked at build or run time

- **Qt 6**: used under the GNU LGPL v3. See https://www.qt.io/licensing.
  Windows builds deploy the Qt DLLs unmodified next to the executable.
- **libiio** (optional, for ADALM-PLUTO): LGPL-2.1. Analog Devices, https://github.com/analogdevicesinc/libiio
- **librtlsdr** (optional, loaded at run time from `rtlsdr.dll`): GPL-2.0-or-later. Not distributed with this source.

## Material derived from SatDump (GPL-3.0)

These tables come from or were checked against [SatDump](https://github.com/SatDump/SatDump)
(GPL-3.0-or-later). GPL-3.0 code and data may be combined with an AGPL-3.0 work, as section 13
of both licenses allows. They stay under GPL-3.0 terms.

- `resources/fy2/svissr/lut.png`: the FY-2 S-VISSR legacy false-colour lookup table distributed with
  SatDump. SatDump credits @eswnl and the original [FY-2x_CLUT](https://github.com/eswnl/FY-2x_CLUT)
  project. The table was converted to PNG with permission.
- `src/gvar_fc_lut.inc`: the GOES GVAR false-colour table (`lut/goes/gvar/fc_lut.png` in SatDump).
- `src/mwcal_coefs.inc`: microwave-sounder calibration coefficients (EUMETSAT/NOAA instrument data,
  as compiled in SatDump's calibration files). Numbers only.

Wherever comments say a format or pipeline was "checked against SatDump", this project contains
an independent implementation, not SatDump source code.

## Maps and imagery (`resources/maps/`)

- `world_relief.jpg`: NASA Blue Marble relief imagery (public domain, NASA Earth Observatory),
  in the equirectangular form distributed with SatDump.
- `landmask.jpg`: land/sea mask derived from the land mask distributed with SatDump.
- `ne_10m_admin_0_countries_chn.geojson`: [Natural Earth](https://www.naturalearthdata.com/)
  1:10m Admin 0 Countries, China point-of-view version, simplified to about 0.008°. Public domain.
  It is used for border overlays and is not part of satellite navigation.

## Facts used without code

- ELEKTRO-L GGAK SKIF/SKL record maps and scalings (`src/ggak.cpp`) follow reverse-engineering
  results that others have published. Raw counts are always kept next to the scaled values.
- Downlink formats follow the public operator documents: CMA/NSMC S-VISSR, KMA LRIT/HRIT, CGMS
  LRIT/HRIT, the NOAA GOES-N databook, the EUMETSAT MetOp AHRPT and CCSDS standards.

## Satellite data

Imagery and data received with this software belong to their operators: CMA/NSMC (FY-2),
KMA (GK-2A), Roshydromet (ELEKTRO-L, METEOR-M), NOAA (GOES) and EUMETSAT (MetOp).
