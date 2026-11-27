#!/usr/bin/env python3
"""Package imported official FY-2 regional L2 rasters as self-describing CF NetCDF."""
from __future__ import annotations

import json
import argparse
import re
from pathlib import Path

import netCDF4
import numpy as np
from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
SCENES = [
    "20261005_000146_400",
    "20261005_005526_321",
    "20261005_020148_029",
    "20261005_030145_858",
    "20261005_035949_083",
]
PRODUCTS = {
    "official_sst": ("sst", "sea_surface_temperature", "degree_Celsius"),
    "official_pre": ("precipitation", "lwe_thickness_of_precipitation_amount", "mm"),
    "official_uth": ("uth", "relative_humidity", "%"),
    "official_olr": ("olr", "toa_outgoing_longwave_flux", "W m-2"),
}
ENCODING = re.compile(
    r"physical value = \(pixel - ([+-]?[0-9.]+)\) / ([+-]?[0-9.]+);"
)
FILL_FLOAT = np.float32(-9999.0)
TIME_RE = re.compile(r"nominal time\s+(\d{4}-\d{2}-\d{2})\s+(\d{2}:\d{2})\s+UTC", re.I)


def nominal_time_utc(note: str) -> str:
    match = TIME_RE.search(note)
    return f"{match.group(1)}T{match.group(2)}:00Z" if match else ""


def read_grid(scene: Path, sidecar: dict, data_path: Path) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray, float, float]:
    image = np.asarray(Image.open(data_path))
    if image.ndim != 2 or image.dtype != np.uint16:
        raise ValueError(f"expected uint16 single-band PNG: {data_path} ({image.shape}, {image.dtype})")
    height, width = image.shape
    pgw_path = data_path.with_name(data_path.name + ".pgw")
    prj_path = data_path.with_name(data_path.name + ".prj")
    if not pgw_path.is_file() or not prj_path.is_file():
        raise ValueError(f"missing georeferencing sidecars for {data_path}")
    pgw = [float(x) for x in pgw_path.read_text(encoding="ascii").split()]
    if len(pgw) != 6 or not np.allclose(pgw[1:3], [0.0, 0.0], atol=1e-12):
        raise ValueError(f"expected an unrotated geographic world file: {data_path}")
    dx, _, _, dy, first_lon, first_lat = pgw
    if dx <= 0.0 or dy >= 0.0:
        raise ValueError(f"unexpected world-file axis direction: {data_path}")
    bounds = {
        "west": first_lon - dx / 2.0,
        "east": first_lon + (width - 0.5) * dx,
        "south": first_lat + (height - 0.5) * dy,
        "north": first_lat - dy / 2.0,
    }
    pix = {"longitude": dx, "latitude": -dy}
    if not np.isclose((bounds["east"] - bounds["west"]) / width, pix["longitude"], atol=1e-10):
        raise ValueError(f"longitude pixel size disagrees with bounds: {data_path}")
    if not np.isclose((bounds["north"] - bounds["south"]) / height, pix["latitude"], atol=1e-10):
        raise ValueError(f"latitude pixel size disagrees with bounds: {data_path}")
    if "region_bounds_degrees" in sidecar and not all(
        np.isclose(float(sidecar["region_bounds_degrees"][k]), bounds[k], atol=1e-8)
        for k in bounds
    ):
        raise ValueError(f"world file disagrees with sidecar bounds: {data_path}")
    if "pixel_size_degrees" in sidecar and (
        not np.isclose(float(sidecar["pixel_size_degrees"]["longitude"]), dx, atol=1e-10)
        or not np.isclose(float(sidecar["pixel_size_degrees"]["latitude"]), -dy, atol=1e-10)
    ):
        raise ValueError(f"world file disagrees with sidecar pixel size: {data_path}")
    if "AUTHORITY[\"EPSG\",\"4326\"]" not in prj_path.read_text(encoding="ascii"):
        raise ValueError(f"expected EPSG:4326 projection file: {data_path}")
    lat = first_lat + np.arange(height, dtype=np.float64) * dy
    lon = first_lon + np.arange(width, dtype=np.float64) * dx
    offset_scale = ENCODING.search(sidecar["encoding"])
    if not offset_scale:
        raise ValueError(f"unrecognized physical encoding: {sidecar['encoding']}")
    offset, scale = map(float, offset_scale.groups())
    no_data = int(sidecar.get("no_data_value", 65535))
    valid = image != no_data
    physical = np.full(image.shape, FILL_FLOAT, dtype=np.float32)
    physical[valid] = ((image[valid].astype(np.float64) - offset) / scale).astype(np.float32)
    if int(valid.sum()) != int(sidecar["valid_pixel_count"]):
        raise ValueError(f"valid-pixel count disagrees with sidecar: {data_path}")
    low, high = map(float, sidecar["valid_range"])
    if valid.any() and (float(physical[valid].min()) < low - 1e-5 or float(physical[valid].max()) > high + 1e-5):
        raise ValueError(f"decoded value outside declared range: {data_path}")
    # Backfill missing spatial metadata from the already verified affine sidecar.
    sidecar.setdefault("region_bounds_degrees", bounds)
    sidecar.setdefault("pixel_size_degrees", pix)
    sidecar.setdefault("coordinate_reference_system", "EPSG:4326")
    sidecar.setdefault("grid_registration", "pixel centres; derived from the .pgw world file")
    return physical, valid, lat, lon, offset, scale


def export_scene(scene_id: str, scene_dir: Path | None = None) -> dict:
    scene = scene_dir if scene_dir is not None else ROOT / "build" / "FY2_DATA" / scene_id
    observation_path = scene / "FY2_Observation.json"
    observation = json.loads(observation_path.read_text(encoding="utf-8-sig"))
    sidecars = sorted(scene.glob("Region_*_Official_*.png.json"))
    if not sidecars:
        raise FileNotFoundError(f"no imported official rasters in {scene}")

    # Several official products in a scene share the same regional pixel grid.
    # If one raster lost its .pgw/.prj, restore it only from a peer raster whose
    # dimensions and manifest grid agree, never from a guessed satellite model.
    peers = [scene / json.loads(p.read_text(encoding="utf-8"))["path"] for p in sidecars]
    peer_dims = {Image.open(p).size for p in peers}
    if len(peer_dims) != 1:
        raise ValueError(f"official rasters do not share one image size in {scene_id}: {peer_dims}")
    canonical = next((p for p in peers if Path(str(p) + ".pgw").is_file()
                      and Path(str(p) + ".prj").is_file()), None)
    if canonical is not None:
        world_bytes = Path(str(canonical) + ".pgw").read_bytes()
        projection_bytes = Path(str(canonical) + ".prj").read_bytes()
        width, height = Image.open(canonical).size
        pgw_values = [float(x) for x in world_bytes.decode("ascii").split()]
        regional = observation.get("regional_projection", {})
        lon_span = float(regional.get("longitude_span_deg", 0.0))
        lat_span = float(regional.get("latitude_span_deg", 0.0))
        center_lon = float(regional.get("center_longitude_deg", 0.0))
        center_lat = float(regional.get("center_latitude_deg", 0.0))
        if len(pgw_values) != 6 or not np.allclose(pgw_values[1:3], [0, 0], atol=1e-12):
            raise ValueError(f"peer world file is not a north-up geographic grid: {canonical}")
        dx, _, _, dy, x0, y0 = pgw_values
        expected = [lon_span / width, 0.0, 0.0, -lat_span / height,
                    center_lon - lon_span / 2 + lon_span / width / 2,
                    center_lat + lat_span / 2 - lat_span / height / 2]
        if not np.allclose(pgw_values, expected, atol=1e-8, rtol=0.0):
            raise ValueError(f"peer affine disagrees with the scene's declared region grid: {canonical}")
        for peer in peers:
            for ext, payload in ((".pgw", world_bytes), (".prj", projection_bytes)):
                georef_path = Path(str(peer) + ext)
                if not georef_path.exists():
                    georef_path.write_bytes(payload)
                elif georef_path.read_bytes() != payload:
                    raise ValueError(f"official rasters have conflicting {ext} files in {scene_id}")

    fields = []
    reference = None
    for sidecar_path in sidecars:
        meta = json.loads(sidecar_path.read_text(encoding="utf-8"))
        key = meta.get("product_type")
        if key not in PRODUCTS:
            raise ValueError(f"unsupported official product type {key!r}: {sidecar_path}")
        var_name, standard_name, units = PRODUCTS[key]
        raster_path = scene / meta["path"]
        physical, valid, lat, lon, offset, scale = read_grid(scene, meta, raster_path)
        sidecar_path.write_text(json.dumps(meta, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        grid = (physical.shape, lat, lon, meta["region_bounds_degrees"], meta["pixel_size_degrees"])
        if reference is None:
            reference = grid
        elif (grid[0] != reference[0] or not np.array_equal(lat, reference[1])
              or not np.array_equal(lon, reference[2]) or grid[3:] != reference[3:]):
            raise ValueError(f"official products do not share one regional grid in {scene_id}")
        fields.append((var_name, standard_name, units, meta, physical, valid, offset, scale))

    assert reference is not None
    shape, lat, lon, bounds, pixel_size = reference
    output = scene / f"FY2G_L2_Official_{scene_id}.nc"
    dictionary = output.with_suffix(".dictionary.json")
    working_output = output.with_name(output.stem + ".tmp.nc")
    if working_output.exists():
        working_output.unlink()
    with netCDF4.Dataset(working_output, "w", format="NETCDF4_CLASSIC") as ds:
        ds.createDimension("y", shape[0])
        ds.createDimension("x", shape[1])
        lat_var = ds.createVariable("latitude", "f8", ("y",))
        lon_var = ds.createVariable("longitude", "f8", ("x",))
        lat_var[:] = lat
        lon_var[:] = lon
        lat_var.standard_name = "latitude"
        lat_var.long_name = "latitude at pixel centre"
        lat_var.units = "degrees_north"
        lat_var.axis = "Y"
        lon_var.standard_name = "longitude"
        lon_var.long_name = "longitude at pixel centre"
        lon_var.units = "degrees_east"
        lon_var.axis = "X"

        crs = ds.createVariable("crs", "i4")
        crs.assignValue(0)
        crs.grid_mapping_name = "latitude_longitude"
        crs.longitude_of_prime_meridian = np.float64(0.0)
        crs.semi_major_axis = np.float64(6378137.0)
        crs.inverse_flattening = np.float64(298.257223563)
        crs.spatial_ref = (
            'GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563]],'
            'PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433],AUTHORITY["EPSG","4326"]]'
        )
        crs.crs_wkt = crs.spatial_ref

        ds.Conventions = "CF-1.8"
        ds.title = "Official FY-2G L2 physical products on the station regional grid"
        ds.satellite = "FY-2G"
        ds.instrument = observation.get("instrument", "FY-2G S-VISSR")
        ds.processing_level = "L2"
        ds.product_source = "Official NSMC HDF L2 fields; values decoded without empirical rescaling"
        ds.grid_registration = "pixel centres"
        ds.geospatial_bounds = (
            f"[{bounds['west']}, {bounds['south']}, {bounds['east']}, {bounds['north']}]"
        )
        ds.geospatial_lon_resolution = np.float64(pixel_size["longitude"])
        ds.geospatial_lat_resolution = np.float64(pixel_size["latitude"])
        ds.spatial_ref = "EPSG:4326"
        ds.product_times_are_per_variable = "true; each variable has its own official nominal time"
        ds.scene_id = scene_id
        ds.station_observation_complete_disk = np.int8(bool(observation.get("complete_disk", False)))
        ds.comment = (
            "This file may contain official L2 fields from different nominal product times. "
            "Read each variable's source_product_time_utc and temporal_support attributes. "
            "Geolocation is reconstructed from the official FY-2 NOM grid and station-region transform."
        )

        variable_records = []
        for name, standard_name, units, meta, physical, valid, offset, scale in fields:
            var = ds.createVariable(
                name, "f4", ("y", "x"), fill_value=FILL_FLOAT,
                zlib=True, complevel=4, shuffle=True,
            )
            var[:] = physical
            var.standard_name = standard_name
            var.long_name = meta.get("source_layer", meta.get("display_name", name))
            var.units = units
            var.grid_mapping = "crs"
            var.coordinates = "latitude longitude"
            var.valid_min = np.float32(meta["valid_range"][0])
            var.valid_max = np.float32(meta["valid_range"][1])
            var.source_product_type = meta["product_type"]
            var.source_product_time_utc = nominal_time_utc(meta.get("temporal_note", ""))
            var.temporal_support = meta.get("temporal_note", "not specified")
            var.source_hdf_file = Path(meta["product_source"]).name
            var.source_layer = meta.get("source_layer", "")
            var.quality_index = np.int32(meta.get("quality_index", -1))
            var.source_fill_value = np.int32(meta.get("no_data_value", 65535))
            var.source_scale_factor = np.float64(1.0 / scale)
            var.source_pixel_offset = np.float64(-offset / scale)
            var.projection_method = meta.get("projection", "")
            var.projection_assumption = meta.get("projection_assumption", "")
            var.source_disk_half = meta.get("source_disk_half", "")
            var.method_and_limitations = meta.get("method_and_limitations", "")
            variable_records.append({
                "variable": name,
                "source_product_type": meta["product_type"],
                "source_hdf": meta["product_source"],
                "source_layer": meta.get("source_layer", ""),
                "units": units,
                "source_product_time_utc": nominal_time_utc(meta.get("temporal_note", "")),
                "temporal_support": meta.get("temporal_note", ""),
                "quality_index": meta.get("quality_index", -1),
                "valid_pixel_count": int(valid.sum()),
                "valid_range": meta["valid_range"],
                "no_data_value": float(FILL_FLOAT),
                "source_encoding": meta["encoding"],
                "source_projection_assumption": meta.get("projection_assumption", ""),
            })

    # Reopen via netCDF4 and verify field masks, values, and coordinates against the PNG sources.
    with netCDF4.Dataset(working_output, "r") as ds:
        if ds.data_model != "NETCDF4_CLASSIC" or ds.Conventions != "CF-1.8":
            raise AssertionError(f"unexpected NetCDF model/conventions in {output}")
        if not np.array_equal(ds.variables["latitude"][:], lat) or not np.array_equal(ds.variables["longitude"][:], lon):
            raise AssertionError(f"coordinate round trip failed for {output}")
        for name, _, _, meta, physical, valid, _, _ in fields:
            actual = np.ma.asarray(ds.variables[name][:])
            if int(actual.count()) != int(valid.sum()):
                raise AssertionError(f"valid mask mismatch for {scene_id}/{name}")
            if not np.array_equal(np.ma.getmaskarray(actual), ~valid):
                raise AssertionError(f"nodata mask mismatch for {scene_id}/{name}")
            if not np.array_equal(actual.compressed(), physical[valid]):
                raise AssertionError(f"physical value round trip failed for {scene_id}/{name}")
    working_output.replace(output)

    dictionary.write_text(json.dumps({
        "format": "NETCDF4_CLASSIC",
        "conventions": "CF-1.8",
        "satellite": "FY-2G",
        "scene_id": scene_id,
        "grid": {
            "crs": "EPSG:4326",
            "registration": "pixel centres",
            "shape_y_x": list(shape),
            "bounds_west_south_east_north": [bounds["west"], bounds["south"], bounds["east"], bounds["north"]],
            "pixel_size_degrees": pixel_size,
        },
        "variables": variable_records,
        "validation": "Reopened with netCDF4; each variable's mask and decoded float values match its uint16 source PNG exactly; coordinates match sidecar grid.",
        "limitations": [
            "L2 variables can have different nominal product times; inspect each variable attribute.",
            "Projection is reconstructed from the official FY-2 NOM grid and station region metadata.",
            "This validates packaging and value preservation, not meteorological accuracy.",
        ],
    }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    products = observation.setdefault("products", [])
    for path in (output, dictionary):
        relative = path.name
        products = [item for item in products if item.get("path") != relative]
        products.append({"path": relative, "absolute_path": str(path.resolve()).replace("\\", "/")})
    observation["products"] = products
    observation_path.write_text(json.dumps(observation, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return {"scene": scene_id, "file": str(output), "bytes": output.stat().st_size,
            "variables": [r["variable"] for r in variable_records],
            "valid_pixels": {r["variable"]: r["valid_pixel_count"] for r in variable_records}}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene_ids", nargs="*", help="scene directory IDs; defaults to the currently imported FY-2G set")
    args = parser.parse_args()
    results = [export_scene(scene) for scene in (args.scene_ids or SCENES)]
    print(json.dumps(results, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
