"""Write a single-channel calibrated GK-2A LRIT full disk as CF NetCDF-3.

The count lookup table and GEOS navigation are read from the accompanying LRIT file.
Navigation follows the KMA GK-2A LRIT header and CGMS LRIT/HRIT GEOS equations; the
script deliberately refuses to infer a scale from the image limb.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
from datetime import datetime, timezone
from pathlib import Path

import netCDF4
import numpy as np
from PIL import Image


def parse_lrit(path: Path) -> tuple[dict, np.ndarray, str, str]:
    raw = path.read_bytes()
    if len(raw) < 16:
        raise ValueError("LRIT file is shorter than its primary header")
    header_end = int.from_bytes(raw[4:8], "big")
    if header_end < 16 or header_end > len(raw):
        raise ValueError("LRIT header length is invalid")
    nav = None
    table = np.full(256, np.nan, dtype=np.float32)
    channel = "unknown"
    units = "unknown"
    pos = 0
    while pos + 3 <= header_end:
        typ = raw[pos]
        size = int.from_bytes(raw[pos + 1:pos + 3], "big")
        if size < 3 or pos + size > header_end:
            raise ValueError(f"invalid LRIT record at byte {pos}")
        record = raw[pos + 3:pos + size]
        if typ == 2 and size >= 51:
            projection = record[:32].decode("ascii", errors="ignore").strip(" \0")
            if not projection.upper().startswith("GEOS(") or not projection.endswith(")"):
                raise ValueError(f"unsupported LRIT projection {projection!r}")
            nav = {
                "sub_lon": float(projection[5:-1]),
                "cfac": struct.unpack(">i", record[32:36])[0],
                "lfac": struct.unpack(">i", record[36:40])[0],
                "coff": struct.unpack(">i", record[40:44])[0],
                "loff": struct.unpack(">i", record[44:48])[0],
            }
        elif typ == 3:  # KMA image data function record carries the TBB conversion table.
            text = record.decode("ascii", errors="ignore")
            for line in text.splitlines():
                if ":=" not in line:
                    continue
                key, value = line.split(":=", 1)
                key, value = key.strip(), value.strip()
                if key == "_NAME":
                    channel = value
                elif key == "_UNIT":
                    units = value
                elif key.isdigit() and int(key) < table.size:
                    try:
                        table[int(key)] = float(value)
                    except ValueError:
                        pass
        pos += size
    if nav is None or nav["cfac"] == 0 or nav["lfac"] == 0:
        raise ValueError("missing/invalid LRIT GEOS navigation")
    if np.isfinite(table).sum() < 16:
        raise ValueError("LRIT count-to-temperature table is missing")
    return nav, table, channel, units


def geolocation(shape: tuple[int, int], nav: dict) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Inverse projection from CGMS LRIT/HRIT Global Specification, 4.4.3.2."""
    height, width = shape
    xdeg = (np.arange(width, dtype=np.float64) - nav["coff"]) * 65536.0 / nav["cfac"]
    ydeg = (np.arange(height, dtype=np.float64) - nav["loff"]) * 65536.0 / nav["lfac"]
    x, y = np.meshgrid(np.deg2rad(xdeg), np.deg2rad(ydeg))
    cosx, sinx = np.cos(x), np.sin(x)
    cosy, siny = np.cos(y), np.sin(y)
    ecc = 1.006739501
    denom = cosy * cosy + ecc * siny * siny
    term = 42164.0 * cosx * cosy
    disc = term * term - denom * (42164.0**2 - 6378.137**2)
    visible = disc >= 0.0
    sd = np.sqrt(np.maximum(disc, 0.0))
    sn = (term - sd) / denom
    s1 = 42164.0 - sn * cosx * cosy
    s2 = sn * sinx * cosy
    s3 = -sn * siny
    lat = np.rad2deg(np.arctan(ecc * s3 / np.sqrt(s1 * s1 + s2 * s2)))
    lon = nav["sub_lon"] + np.rad2deg(np.arctan2(s2, s1))
    lon = (lon + 180.0) % 360.0 - 180.0
    lat[~visible] = np.nan
    lon[~visible] = np.nan
    return lat.astype(np.float32), lon.astype(np.float32), visible


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("counts_png", type=Path, help="full-disk 8-bit count image")
    parser.add_argument("lrit", type=Path, help="matching LRIT segment with type-2/type-3 headers")
    parser.add_argument("output_nc", type=Path)
    args = parser.parse_args()

    nav, table, channel, units = parse_lrit(args.lrit)
    counts = np.asarray(Image.open(args.counts_png).convert("L"), dtype=np.uint8)
    if counts.ndim != 2 or counts.size == 0:
        raise ValueError("input is not a grayscale full-disk image")
    lat, lon, visible = geolocation(counts.shape, nav)
    valid = visible & (counts != 0) & np.isfinite(table[counts])
    valid &= (table[counts] >= 150.0) & (table[counts] <= 350.0)
    temperature = np.full(counts.shape, np.float32(-9999.0), dtype=np.float32)
    temperature[valid] = table[counts[valid]]
    quality = np.zeros(counts.shape, dtype=np.uint8)
    quality[~visible] |= 1
    quality[visible & ~valid] |= 2
    row_has_sample = (valid & visible).any(axis=1)
    missing_rows = visible.any(axis=1) & ~row_has_sample
    for row in np.flatnonzero(missing_rows):
        quality[row, visible[row]] |= 4
    received_fraction = float(((counts != 0) & visible).sum() / max(1, visible.sum()))
    complete_disk = received_fraction >= 0.98
    earth_pixels = int(visible.sum())
    valid_earth_pixels = int((valid & visible).sum())
    earth_rows = visible.any(axis=1)
    received_rows = (valid & visible).any(axis=1)
    quality_summary = {
        "schema": "mgas-observation-quality/1.0",
        "satellite": "GK-2A",
        "channel": channel,
        "assessment_scope": "observation completeness and processing state; not geolocation or retrieval accuracy",
        "earth_disk_pixels": earth_pixels,
        "valid_calibrated_pixels": valid_earth_pixels,
        "valid_earth_pixel_fraction": received_fraction,
        "earth_rows": int(earth_rows.sum()),
        "rows_with_valid_samples": int(received_rows.sum()),
        "missing_earth_rows": int(missing_rows.sum()),
        "off_disk_pixels": int((~visible).sum()),
        "earth_pixels_without_valid_calibration": int(earth_pixels - valid_earth_pixels),
        "quality_flag_pixel_counts": {
            "off_disk": int((quality & 1 != 0).sum()),
            "no_valid_calibrated_sample": int((quality & 2 != 0).sum()),
            "missing_scanline_row": int((quality & 4 != 0).sum()),
        },
        "complete_disk_threshold": 0.98,
        "observation_status": "complete" if complete_disk else "partial",
        "status_rule": "complete when valid calibrated Earth-disk pixel fraction is at least 0.98",
        "calibration_table_entries": int(np.isfinite(table).sum()),
        "quality_is_accuracy_score": False,
    }
    created_utc = datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")
    lat[~visible] = -9999.0
    lon[~visible] = -9999.0

    args.output_nc.parent.mkdir(parents=True, exist_ok=True)
    with netCDF4.Dataset(args.output_nc, "w", format="NETCDF3_CLASSIC") as ds:
        ds.createDimension("y", counts.shape[0])
        ds.createDimension("x", counts.shape[1])
        ds.Conventions = "CF-1.8"
        ds.title = f"GK-2A LRIT {channel} single-channel calibrated full disk"
        ds.satellite = "GK-2A"
        ds.instrument = "AMI via LRIT"
        ds.product_level = "station local single-channel Level 1 calibrated raster"
        ds.navigation_source = "LRIT Image Navigation type 2; CGMS GEOS projection equations"
        ds.calibration_source = "LRIT Image Data Function type 3 count-to-temperature table"
        ds.history = f"created {created_utc} by tools/gk2a_lrit_l1b.py"
        ds.source_counts_image = args.counts_png.name
        ds.source_lrit_header = args.lrit.name
        ds.lrit_cfac = np.int32(nav["cfac"])
        ds.lrit_lfac = np.int32(nav["lfac"])
        ds.lrit_coff = np.int32(nav["coff"])
        ds.lrit_loff = np.int32(nav["loff"])
        ds.sub_satellite_longitude = np.float64(nav["sub_lon"])
        ds.received_sample_fraction_on_earth = np.float64(received_fraction)
        ds.complete_disk = np.int8(1 if complete_disk else 0)
        ds.missing_scanline_rows_detected = np.int32(int(missing_rows.sum()))
        ds.quality_flag_bits = (
            "1=outside visible Earth disk; 2=no usable calibrated sample; "
            "4=Earth-view row with no calibrated sample (values remain fill)"
        )

        y = ds.createVariable("y", "i4", ("y",))
        x = ds.createVariable("x", "i4", ("x",))
        y[:] = np.arange(counts.shape[0], dtype=np.int32)
        x[:] = np.arange(counts.shape[1], dtype=np.int32)
        for name, values, units_name, long_name in (
            ("latitude", lat, "degrees_north", "pixel-centre latitude"),
            ("longitude", lon, "degrees_east", "pixel-centre longitude"),
        ):
            var = ds.createVariable(name, "f4", ("y", "x"), fill_value=np.float32(-9999.0))
            var.units = units_name
            var.long_name = long_name
            var[:] = values
        bt = ds.createVariable("brightness_temperature", "f4", ("y", "x"),
                               fill_value=np.float32(-9999.0))
        bt.units = "K"
        bt.long_name = f"{channel} brightness temperature"
        bt.calibration_units_from_lrit = units
        bt[:] = temperature
        qf = ds.createVariable("quality_flag", "i1", ("y", "x"))
        qf.flag_masks = np.array([1, 2, 4], dtype=np.int8)
        qf.flag_meanings = "off_disk no_valid_calibrated_sample missing_scanline_row"
        qf[:] = quality
    def sha256(path: Path) -> str:
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()

    dictionary = {
        "format": "GK-2A LRIT calibrated single-channel L1B, NetCDF-3 classic",
        "product_level": "station local, single-channel calibrated raster; not a full AMI multispectral product",
        "satellite": "GK-2A",
        "instrument": "AMI via LRIT",
        "channel": channel,
        "units": units,
        "dimensions": {"y": int(counts.shape[0]), "x": int(counts.shape[1])},
        "complete_disk": complete_disk,
        "received_sample_fraction_on_earth": received_fraction,
        "missing_scanline_rows_detected": int(missing_rows.sum()),
        "observation_quality_summary": quality_summary,
        "observation_quality_report": args.output_nc.with_suffix(".quality.json").name,
        "navigation": {"projection": "GEOS", **nav,
                       "source": f"LRIT Image Navigation type 2: {args.lrit.name}"},
        "calibration": {"source": "LRIT Image Data Function type 3 count-to-temperature table",
                        "entries": int(np.isfinite(table).sum()),
                        "table_units": units},
        "quality_flag_bits": {
            "1": "outside visible Earth disk",
            "2": "no usable calibrated sample",
            "4": "Earth-view row with no calibrated sample; values remain fill",
        },
        "variables": {
            "latitude": "pixel-centre geodetic latitude, degrees_north; fill -9999 outside disk",
            "longitude": "pixel-centre longitude, degrees_east; fill -9999 outside disk",
            "brightness_temperature": f"{channel} brightness temperature, kelvin; fill -9999",
            "quality_flag": "bit mask described by quality_flag_bits",
        },
        "source_files": {
            "counts_image": {"name": args.counts_png.name, "sha256": sha256(args.counts_png)},
            "lrit_header": {"name": args.lrit.name, "sha256": sha256(args.lrit)},
        },
        "netcdf_file": {"name": args.output_nc.name, "sha256": sha256(args.output_nc)},
        "created_utc": created_utc,
    }
    dictionary_path = args.output_nc.with_suffix(".dictionary.json")
    dictionary_path.write_text(json.dumps(dictionary, ensure_ascii=False, indent=2) + "\n",
                               encoding="utf-8")
    quality_path = args.output_nc.with_suffix(".quality.json")
    quality_path.write_text(json.dumps(quality_summary, ensure_ascii=False, indent=2) + "\n",
                            encoding="utf-8")
    print(f"wrote {args.output_nc}; {channel}, {int(valid.sum())} calibrated pixels; "
          f"Earth coverage {received_fraction:.4%}; complete_disk={complete_disk}; "
          f"missing rows {int(missing_rows.sum())}; navigation {nav}")
    print(f"wrote {dictionary_path}")
    print(f"wrote {quality_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
