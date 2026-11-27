#!/usr/bin/env python3
"""Reproject official FY-2 HDF regional products onto a station observation grid."""
import argparse
import json
import math
from pathlib import Path
import re
import sys
from datetime import datetime, timezone

import netCDF4
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from export_official_l2_cf import export_scene as export_cf_netcdf

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "work" / "fy2verify"))
import georeg  # noqa: E402 -- share the station's verified WGS-84 view-angle transform


STOPS = [
    (35, 70, 170), (30, 170, 210), (65, 190, 110),
    (245, 210, 55), (225, 75, 45),
]


def pearson_correlation_chunked(x: np.ndarray, y: np.ndarray, chunk_size: int = 65536) -> float:
    """Pearson r with bounded temporaries; large np.corrcoef calls crash this runtime."""
    if x.size != y.size or x.size == 0:
        return float("nan")
    mean_x = float(np.sum(x, dtype=np.float64) / x.size)
    mean_y = float(np.sum(y, dtype=np.float64) / y.size)
    covariance = variance_x = variance_y = 0.0
    for start in range(0, x.size, chunk_size):
        dx = x[start:start + chunk_size] - mean_x
        dy = y[start:start + chunk_size] - mean_y
        covariance += float(np.sum(dx * dy, dtype=np.float64))
        variance_x += float(np.sum(dx * dx, dtype=np.float64))
        variance_y += float(np.sum(dy * dy, dtype=np.float64))
    denominator = math.sqrt(variance_x * variance_y)
    return covariance / denominator if denominator > 0.0 else float("nan")


def colorize(values, valid, low, high):
    t = np.clip((values - low) / (high - low), 0.0, 1.0)
    scaled = t * (len(STOPS) - 1)
    idx = np.minimum(scaled.astype(np.int32), len(STOPS) - 2)
    frac = (scaled - idx)[..., None]
    stops = np.asarray(STOPS, dtype=np.float32)
    rgb = np.rint(stops[idx] * (1.0 - frac) + stops[idx + 1] * frac).astype(np.uint8)
    rgb[~valid] = (115, 125, 138)
    return rgb


def add_legend(rgb, caption, unit, low, high):
    h, w, _ = rgb.shape
    canvas = Image.new("RGB", (w, h + 78), (12, 18, 25))
    canvas.paste(Image.fromarray(rgb), (0, 0))
    draw = ImageDraw.Draw(canvas)
    font = ImageFont.load_default()
    draw.text((18, h + 7), f"{caption}; {unit}", fill=(235, 240, 246), font=font)
    left, top, width, bar_h = 20, h + 39, min(460, w - 40), 18
    ramp = colorize(np.linspace(low, high, width)[None, :], np.ones((1, width), bool), low, high)[0]
    canvas.paste(Image.fromarray(ramp), (left, top))
    for tick in range(5):
        value = low + (high - low) * tick / 4
        draw.text((left + width * tick // 4 - 10, top + bar_h + 3), f"{value:g}", fill=(235, 240, 246), font=font)
    return canvas


def official_to_region(path, variable_name, width, height, center_lat, center_lon,
                       lon_span, lat_span, satellite_lon):
    with netCDF4.Dataset(path) as ds:
        var = ds.variables[variable_name]
        raw = np.ma.filled(var[:], np.nan).astype(np.float64)
        if raw.ndim != 2:
            raise ValueError(f"Expected a 2-D product in {path}: {variable_name}")
        low = float(getattr(var, "LowerValidRange"))
        high = float(getattr(var, "UpperValidRange"))
        fill = getattr(var, "FillValue", None)
        units = str(getattr(var, "Unit", ""))
        layer = str(getattr(var, "LayerName", variable_name))
        quality = int(getattr(var, "QualityIndex", -1))

    # FY-2 nominal grid: geostationary view angles over the full Earth disk.
    rows, cols = raw.shape
    # FY-2 02-batch NOM products use the documented 0.00014-rad angular grid.
    # PRE is 1144x2288: full-width sampling, half-rate row stepping. Its HDF
    # layer does not say which disk half it stores, so compare both legal row
    # offsets against the layer's own valid-range/fill metadata below.
    step_x = step_y = 0.00014
    if rows * 2 == cols:
        step_y *= 2.0
    lons = center_lon - lon_span / 2 + (np.arange(width) + 0.5) * lon_span / width
    lats = center_lat + lat_span / 2 - (np.arange(height) + 0.5) * lat_span / height
    lon_grid, lat_grid = np.meshgrid(lons, lats)
    x_angle, y_angle = georeg.lonlat_to_angles(lon_grid, lat_grid, satellite_lon)
    # Match the station's ellipsoidal visibility test: dot the satellite-to-
    # surface vector with the local geodetic surface normal.
    phi = np.deg2rad(lat_grid)
    lam = np.deg2rad(lon_grid)
    eccentricity2 = 6.69437999014e-3
    prime_vertical = 6378.137 / np.sqrt(1.0 - eccentricity2 * np.sin(phi) ** 2)
    px = prime_vertical * np.cos(phi) * np.cos(lam)
    py = prime_vertical * np.cos(phi) * np.sin(lam)
    pz = prime_vertical * (1.0 - eccentricity2) * np.sin(phi)
    sat_lon = math.radians(satellite_lon)
    sx, sy, sz = 42164.0 * math.cos(sat_lon), 42164.0 * math.sin(sat_lon), 0.0
    visible = ((sx - px) * np.cos(phi) * np.cos(lam)
               + (sy - py) * np.cos(phi) * np.sin(lam)
               + (sz - pz) * np.sin(phi)) > 0.0
    source_x = np.rint(x_angle / step_x + (cols - 1) / 2).astype(np.int32)
    row_centres = [((rows - 1) / 2.0, "full disk")]
    if rows * 2 == cols:
        row_centres = [((rows - 1) / 2.0, "north half"),
                       (-(rows - 1) / 2.0, "south half")]
    best = None
    for row_centre, source_half in row_centres:
        source_y = np.rint(row_centre - y_angle / step_y).astype(np.int32)
        inside = visible & (source_x >= 0) & (source_x < cols) & (source_y >= 0) & (source_y < rows)
        output = np.full((height, width), np.nan, dtype=np.float32)
        output[inside] = raw[source_y[inside], source_x[inside]].astype(np.float32)
        valid = inside & np.isfinite(output) & (output >= low) & (output <= high)
        if fill is not None:
            for fill_value in np.asarray(fill).flat:
                valid &= output != float(fill_value)
        count = int(valid.sum())
        if best is None or count > best[0]:
            best = (count, output, valid, source_half)
    assert best is not None
    _, output, valid, source_half = best
    return output, valid, units, layer, quality, source_half, list(raw.shape)


def nominal_time(path):
    if not path:
        return "not imported"
    match = re.search(r"_(\d{8})_(\d{4})\.hdf$", Path(path).name, re.IGNORECASE)
    if not match:
        return "nominal time unavailable from filename"
    return f"{match.group(1)[:4]}-{match.group(1)[4:6]}-{match.group(1)[6:8]} {match.group(2)[:2]}:{match.group(2)[2:]} UTC"


def parse_nominal_time(note):
    match = re.search(r"nominal time\s+(\d{4}-\d{2}-\d{2})\s+(\d{2}:\d{2})\s+UTC", note or "", re.I)
    if not match:
        return None
    return datetime.strptime(f"{match.group(1)} {match.group(2)}", "%Y-%m-%d %H:%M").replace(tzinfo=timezone.utc)


def save_product(directory, manifest, product):
    data = product["data"]
    valid = product["valid"]
    scale = product["scale"]
    offset = product["offset"]
    encoded = np.full(data.shape, 65535, dtype=np.uint16)
    encoded[valid] = np.clip(np.rint(data[valid] * scale + offset), 0, 65534).astype(np.uint16)
    data_path = directory / product["data_name"]
    Image.fromarray(encoded).save(data_path)
    projection = manifest["regional_projection"]
    center_lat = float(projection["center_latitude_deg"])
    center_lon = float(projection["center_longitude_deg"])
    lat_span = float(projection["latitude_span_deg"])
    lon_span = float(projection["longitude_span_deg"])
    height, width = encoded.shape
    pixel_lon = lon_span / width
    pixel_lat = lat_span / height
    west = center_lon - lon_span / 2.0
    north = center_lat + lat_span / 2.0
    # World files register pixel centres, matching official_to_region's grid.
    world_file = data_path.with_name(data_path.name + ".pgw")
    world_file.write_text("\n".join(map(str, (
        pixel_lon, 0.0, 0.0, -pixel_lat,
        west + pixel_lon / 2.0, north - pixel_lat / 2.0,
    ))) + "\n", encoding="ascii")
    prj_file = data_path.with_name(data_path.name + ".prj")
    prj_file.write_text(
        'GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563]],'
        'PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433],AUTHORITY["EPSG","4326"]]\n',
        encoding="ascii")
    enhanced = add_legend(colorize(data, valid, product["display_min"], product["display_max"]),
                          product["caption"], product["unit"],
                          product["display_min"], product["display_max"])
    enhanced_path = directory / product["enhanced_name"]
    enhanced.save(enhanced_path)
    meta = {
        "product_type": product["type"],
        "product_source": product["source"],
        "source_layer": product["layer"],
        "source_units": product["source_units"],
        "units": product["unit"],
        "encoding": f"uint16 grayscale; physical value = (pixel - {offset}) / {scale}; 65535 = no data",
        "valid_range": [product["display_min"], product["display_max"]],
        "projection": "FY-2 geostationary NOM to station equirectangular region; nearest-neighbor sampling",
        "projection_assumption": f"FY-2 satellite subpoint longitude {product['satellite_longitude_deg']} degrees east; nominal full-disk angular sampling",
        "satellite_longitude_deg": product["satellite_longitude_deg"],
        "coordinate_reference_system": "EPSG:4326",
        "grid_registration": "pixel centres; matches the .pgw world file",
        "region_bounds_degrees": {
            "west": west, "east": west + lon_span,
            "south": north - lat_span, "north": north,
        },
        "pixel_size_degrees": {"longitude": pixel_lon, "latitude": pixel_lat},
        "quality_index": product["quality_index"],
        "valid_pixel_count": int(valid.sum()),
        "source_shape": list(product["source_shape"]),
        "source_disk_half": product["source_disk_half"],
        "temporal_note": product["temporal_note"],
        "method_and_limitations": "Official FY-2 product values are copied without empirical rescaling. Geolocation is reconstructed from NOM full-disk geometry; temporal support follows the source product interval.",
    }
    data_meta = dict(meta, display_of_numeric_product=product["data_name"])
    for file, label, note in [
        (data_path, product["caption"], "official physical-value raster"),
        (enhanced_path, product["caption"] + " (color display)", "color map with physical-unit scale"),
    ]:
        sidecar = dict(data_meta if file == enhanced_path else meta,
                       path=file.name, display_name=label, display_note=note)
        file.with_name(file.name + ".json").write_text(json.dumps(sidecar, ensure_ascii=False, indent=2), encoding="utf-8")
        record = {"path": file.name, "absolute_path": str(file.resolve())}
        existing = [x for x in manifest.get("products", []) if x.get("path") != file.name]
        existing.append(record)
        manifest["products"] = existing


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("observation_dir", type=Path)
    ap.add_argument("--pre", type=Path)
    ap.add_argument("--uth", type=Path)
    ap.add_argument("--sst", type=Path)
    ap.add_argument("--olr", type=Path)
    ap.add_argument("--center-lat", type=float, default=None)
    ap.add_argument("--center-lon", type=float, default=None)
    ap.add_argument("--satellite-lon", type=float, default=104.7)
    args = ap.parse_args()
    if not any((args.pre, args.uth, args.sst, args.olr)):
        ap.error("provide at least one official product source: --pre/--uth/--sst/--olr")
    directory = args.observation_dir
    manifest_path = directory / "FY2_Observation.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    projection = manifest["regional_projection"]
    center_lat = args.center_lat if args.center_lat is not None else projection["center_latitude_deg"]
    center_lon = args.center_lon if args.center_lon is not None else projection["center_longitude_deg"]
    projection["center_latitude_deg"] = center_lat
    projection["center_longitude_deg"] = center_lon
    created = datetime.fromisoformat(manifest["created_utc"].replace("Z", "+00:00"))
    start = datetime.strptime(directory.name, "%Y%m%d_%H%M%S_%f").replace(tzinfo=timezone.utc)
    observation_midpoint = start + (created - start) / 2
    lat_span = float(projection["latitude_span_deg"])
    lon_span = float(projection["longitude_span_deg"])
    width, height = int(manifest["dimensions"]["ir_width"] * 68 / 114.55), int(manifest["dimensions"]["ir_height"] * 42 / 114.55)
    # The renderer's saved regional maps define the exact output dimensions.
    existing = next(directory.glob("Region_CH4_IR1_TBB_0p01K.png"), None)
    if existing:
        with Image.open(existing) as image:
            width, height = image.size
    definitions = [
        ("PRE", args.pre, "FY2G PRE 1-Hour Product", "Region_PRE_Official_0p01mm.png", "Region_PRE_Enhanced.png", "PRE", "mm", 100.0, 0.0, 0, 30, 0, "1-hour precipitation accumulation; nominal time " + nominal_time(args.pre)),
        ("UTH", args.uth, "UTH", "Region_UTH_Official_1pct.png", "Region_UTH_Enhanced.png", "UTH", "%", 1.0, 0.0, 0, 100, 0, "multi-layer product; nominal time " + nominal_time(args.uth)),
        ("SST", args.sst, "FY2G SST 3-Hour Product", "Region_SST_Official_0p01C.png", "Region_SST_Enhanced.png", "SST", "°C", 100.0, 200.0, -2, 40, 0, "3-hour product; nominal time " + nominal_time(args.sst)),
        ("OLR", args.olr, "FY2G OLR Hourly Product", "Region_OLR_Official_0p1Wm2.png", "Region_OLR_Enhanced.png", "OLR", "W/m²", 10.0, 0.0, 40, 420, 2, "hourly product; nominal time " + nominal_time(args.olr)),
    ]
    imported_kinds = set()
    for kind, source, layer_name, data_name, enhanced_name, caption, unit, scale, offset, dmin, dmax, quality, temporal in definitions:
        if source is None:
            continue
        imported_kinds.add(kind)
        values, valid, source_units, actual_layer, quality_index, source_disk_half, source_shape = official_to_region(
            source, layer_name, width, height, center_lat, center_lon,
            lon_span, lat_span, args.satellite_lon)
        if kind == "PRE": valid &= (values >= 0) & (values <= 50)
        if kind == "UTH": valid &= (values >= 0) & (values <= 100)
        if kind == "SST": valid &= (values >= -2) & (values <= 40)
        if kind == "OLR": valid &= (values >= 40) & (values <= 420)
        save_product(directory, manifest, {
            "type": f"official_{kind.lower()}", "source": str(source), "layer": actual_layer,
            "source_units": source_units, "unit": unit, "scale": scale, "offset": offset,
            "display_min": dmin, "display_max": dmax, "data_name": data_name,
            "enhanced_name": enhanced_name, "caption": caption, "quality_index": quality_index,
            "source_shape": source_shape, "temporal_note": temporal,
            "satellite_longitude_deg": args.satellite_lon,
            "source_disk_half": source_disk_half,
            "data": values, "valid": valid,
        })
        print(f"{kind}: saved {int(valid.sum())} valid regional pixels from {source.name}")
    source_times = ", ".join(f"{kind} {nominal_time(source)}" for kind, source in
        (("SST", args.sst), ("PRE", args.pre), ("UTH", args.uth), ("OLR", args.olr)) if source)
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    print("[official-import] manifest saved; writing guide and proxy comparisons", flush=True)
    guide_path = directory / "区域产品说明.md"
    if guide_path.exists():
        guide = guide_path.read_text(encoding="utf-8")
        official_rows = {
            "OLR official": "| OLR official | 官方逐小时宽波段 OLR，单位 W/m²；按本次输入 HDF 时刻。 |",
            "SST official": "| SST official | 官方 3 小时海表温度，单位 °C；按本次输入 HDF 时刻。 |",
            "PRE official": "| PRE official | 官方 1 小时累计降水，单位 mm；按本次输入 HDF 时刻。 |",
            "UTH official": "| UTH official | 官方上对流层湿度，单位 %；按本次输入 HDF 时刻。 |",
        }
        lines = guide.splitlines()
        for key, row in official_rows.items():
            kind = key.split()[0]
            if kind not in imported_kinds:
                continue
            found = next((i for i, line in enumerate(lines)
                          if line.startswith(f"| {key} |")), None)
            if found is not None:
                lines[found] = row
                continue
            if key == "OLR official":
                anchor = next((i for i, line in enumerate(lines) if line.startswith("| OLR proxy |")), None)
                insert_at = anchor + 1 if anchor is not None else None
            elif key == "SST official":
                anchor = next((i for i, line in enumerate(lines) if line.startswith("| SST candidate |")), None)
                insert_at = anchor + 1 if anchor is not None else None
            else:
                insert_at = next((i for i, line in enumerate(lines) if line.startswith("| LST proxy |")), None)
            if insert_at is None:
                insert_at = next((i for i, line in enumerate(lines) if line.startswith("|---")), len(lines)) + 1
            lines.insert(insert_at, row)
        guide = "\n".join(lines).rstrip() + "\n"
        official_note = ("官方 HDF L2 栅格按对应 NOM 图层的物理值输出，不是本站反演代理；"
                         f"本次导入时刻：{source_times}。数值 PNG 带 WGS-84 .pgw/.prj。")
        if "官方 HDF L2 栅格按对应 NOM 图层" in guide:
            guide = re.sub(r"官方 HDF L2 栅格按对应 NOM 图层[^\n]*", official_note, guide)
        else:
            guide += "\n" + official_note + "\n"
        guide_path.write_text(guide, encoding="utf-8")
    comparisons = []
    comparison_exclusions = []
    proxy_pairs = [
        ("OLR", "Region_OLR_StefanBoltzmannProxy_0p1Wm2.png", "Region_OLR_Official_0p1Wm2.png", 0.1, 0.0, 0.1, 0.0),
        ("PRE", "Region_PRE_ConvectivePotentialIndex.png", "Region_PRE_Official_0p01mm.png", 1.0, 0.0, 0.01, 0.0),
        ("UTH", "Region_UTH_WaterVaporProxyIndex.png", "Region_UTH_Official_1pct.png", 1.0, 0.0, 1.0, 0.0),
        ("SST", "Region_SST_ClearOceanSplitWindowProxy_0p01K.png", "Region_SST_Official_0p01C.png", 0.01, -273.15, 0.01, -2.0),
    ]
    for kind, proxy_file, official_file, proxy_scale, proxy_offset, official_scale, official_offset in proxy_pairs:
        proxy_path, official_path = directory / proxy_file, directory / official_file
        if not proxy_path.exists() or not official_path.exists():
            continue
        official_sidecar_path = official_path.with_name(official_path.name + ".json")
        official_meta = json.loads(official_sidecar_path.read_text(encoding="utf-8"))
        product_time = parse_nominal_time(official_meta.get("temporal_note", ""))
        gap_minutes = (abs((product_time - observation_midpoint).total_seconds()) / 60.0
                       if product_time is not None else None)
        if gap_minutes is None or gap_minutes > 45.0:
            exclusion = {
                "reason": "official product nominal time is not within the 45-minute scan-midpoint pairing limit",
                "official_nominal_time_utc": (product_time.isoformat().replace("+00:00", "Z")
                                               if product_time is not None else None),
                "observation_scan_midpoint_utc": observation_midpoint.isoformat().replace("+00:00", "Z"),
                "gap_minutes": gap_minutes,
                "maximum_gap_minutes": 45.0,
            }
            official_meta.pop("single_observation_proxy_comparison", None)
            official_meta["proxy_comparison_excluded"] = exclusion
            official_sidecar_path.write_text(json.dumps(official_meta, ensure_ascii=False, indent=2), encoding="utf-8")
            comparison_exclusions.append({"product": kind, **exclusion})
            continue
        print(f"[official-import] compare {kind}", flush=True)
        proxy = np.asarray(Image.open(proxy_path), dtype=np.float64) * proxy_scale + proxy_offset
        official_encoded = np.asarray(Image.open(official_path), dtype=np.float64)
        # SST uses a +2 C offset to encode negative Celsius values in uint16.
        if kind == "SST":
            official = (official_encoded - 200.0) / 100.0
        elif kind == "PRE":
            official = official_encoded / 100.0
        elif kind == "OLR":
            official = official_encoded / 10.0
        else:
            official = official_encoded
        good = np.isfinite(proxy) & np.isfinite(official) & (proxy < 65535) & (official_encoded < 65535)
        if kind == "OLR":
            good &= (proxy >= 40) & (proxy <= 550) & (official >= 40)
        elif kind == "PRE":
            good &= (proxy >= 0) & (proxy <= 100) & (official >= 0)
        elif kind == "UTH":
            good &= (proxy >= 0) & (proxy <= 100) & (official >= 0)
        else:
            good &= (proxy >= -10) & (proxy <= 50) & (official >= -2) & (official <= 40)
        x, y = proxy[good], official[good]
        if not len(x):
            continue
        print(f"[official-import] compare {kind}: {len(x)} pairs", flush=True)
        corr = pearson_correlation_chunked(x, y)
        item = {"product": kind, "pair_count": int(len(x)), "pearson_r": corr}
        item["official_nominal_time_utc"] = product_time.isoformat().replace("+00:00", "Z")
        item["observation_scan_midpoint_utc"] = observation_midpoint.isoformat().replace("+00:00", "Z")
        item["time_gap_minutes"] = gap_minutes
        if kind != "PRE":
            delta = x - y
            item.update({"proxy_minus_official_mean": float(np.mean(delta)),
                         "mae": float(np.mean(np.abs(delta))),
                         "rmse": float(np.sqrt(np.mean(delta * delta)))})
        else:
            pred, obs = x > 0, y > 0
            hits, false_alarm, misses = int(np.sum(pred & obs)), int(np.sum(pred & ~obs)), int(np.sum(~pred & obs))
            item.update({"candidate_event_threshold": ">0 index", "official_event_threshold": ">0 mm",
                         "hits": hits, "false_alarms": false_alarm, "misses": misses,
                         "precision": hits / max(1, hits + false_alarm),
                         "recall": hits / max(1, hits + misses)})
        comparisons.append(item)
        for sidecar in [official_sidecar_path, (directory / proxy_file).with_name(proxy_file + ".json")]:
            if sidecar.exists():
                meta = json.loads(sidecar.read_text(encoding="utf-8"))
                meta["single_observation_proxy_comparison"] = item
                meta.pop("proxy_comparison_excluded", None)
                sidecar.write_text(json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8")
    report_lines = [
        "# 官方 FY-2 产品与本景代理图对比", "",
        "官方 PRE、UTH、SST、OLR 栅格直接按 HDF 物理数值重投影；未将单景回归系数冒充通用定标参数。",
        "本对比使用 equirectangular 区域像元与 NOM 全圆盘近似几何最近邻配准，卫星经度由 --satellite-lon 指定。",
        f"区域扫描中点约 {observation_midpoint:%Y-%m-%d %H:%M} UTC；本次导入产品时刻：{source_times}。各产品时间支撑不完全一致。",
        "", "| 产品 | 配对数 | 空间相关 r | 对比摘要 |", "|---|---:|---:|---|"
    ]
    for item in comparisons:
        if item["product"] == "PRE":
            summary = f"代理候选 precision={item['precision']:.3f}, recall={item['recall']:.3f}; hit/false alarm/miss={item['hits']}/{item['false_alarms']}/{item['misses']}"
        else:
            summary = f"proxy−official bias={item['proxy_minus_official_mean']:.2f}, MAE={item['mae']:.2f}, RMSE={item['rmse']:.2f}"
        report_lines.append(f"| {item['product']} | {item['pair_count']} | {item['pearson_r']:.3f} | {summary} |")
    if comparison_exclusions:
        report_lines += ["", "超出 45 分钟扫描中点配对门限的代理比较已排除，不计入精度指标："]
        for item in comparison_exclusions:
            delta = (f"{item['gap_minutes']:.2f} min" if item["gap_minutes"] is not None else "nominal time unavailable")
            report_lines.append(f"- {item['product']}: official {item['official_nominal_time_utc']}; "
                                f"scene midpoint {item['observation_scan_midpoint_utc']}; gap {delta} (>45 min).")
    report_lines += ["", "PRE 的代理指数不是 mm，precision/recall 只评价候选区与官方非零降水区重叠，不代表降水量误差。", ""]
    (directory / "FY2_Official_Product_Comparison.md").write_text("\n".join(report_lines), encoding="utf-8")
    obsolete = {
        "Region_PRE_ConvectivePotentialIndex.png", "Region_UTH_WaterVaporProxyIndex.png",
        "Region_PRE_Official_Enhanced.png", "Region_PRE_Official_Enhanced.png.json",
        "Region_UTH_Official_Enhanced.png", "Region_UTH_Official_Enhanced.png.json",
        "Region_SST_Official_Enhanced.png", "Region_SST_Official_Enhanced.png.json",
        "Region_OLR_Official_Enhanced.png", "Region_OLR_Official_Enhanced.png.json",
    }
    for name in obsolete:
        (directory / name).unlink(missing_ok=True)
    manifest["products"] = [x for x in manifest.get("products", []) if x.get("path") not in obsolete]
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    print("[official-import] comparison report saved; exporting CF NetCDF", flush=True)
    cf_result = export_cf_netcdf(directory.name, directory)
    print(f"CF NetCDF: wrote {Path(cf_result['file']).name} with {len(cf_result['variables'])} official L2 variable(s)")


if __name__ == "__main__":
    main()
