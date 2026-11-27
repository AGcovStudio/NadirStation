#!/usr/bin/env python3
"""Fetch and cache FY-2G / FY-2H TLEs locally.

Geostationary satellites drift, so the scan geometry must be recomputed from a
current TLE rather than from a hard-coded sub-satellite longitude.  This script
keeps one cached TLE file per satellite under ``resources/orbits`` and records
each element set's epoch so the station can report how stale it is.

Sources are tried in order:

1. CelesTrak GP API (``CATNR`` query)
2. CelesTrak classic TLE endpoint
3. The existing local cache (never overwritten with nothing)

Usage::

    python tools/fetch_tle.py                 # refresh both satellites
    python tools/fetch_tle.py --satellite 2G  # refresh one
    python tools/fetch_tle.py --print         # show what is cached, no network
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
import urllib.request
from pathlib import Path

# The station ships a combined TLE file; keep that as the single source of truth
# for the application while still caching per-satellite records in JSON for the
# verification scripts.
SATELLITES = {
    "2G": {"norad": "40367", "name": "FENGYUN 2G", "longitude_deg": 99.2},
    "2H": {"norad": "43491", "name": "FENGYUN 2H", "longitude_deg": 79.0},
}

SOURCES = [
    "https://celestrak.org/NORAD/elements/gp.php?CATNR={norad}&FORMAT=tle",
    "https://celestrak.com/NORAD/elements/gp.php?CATNR={norad}&FORMAT=tle",
]

TIMEOUT_SECONDS = 25


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def tle_dir() -> Path:
    return repo_root() / "resources" / "orbits"


def combined_path() -> Path:
    return tle_dir() / "fy2.tle"


def cache_path(tag: str) -> Path:
    # tag is "2G"/"2H", so the file is fy2g.tle / fy2h.tle (not fy22g.tle).
    return tle_dir() / f"fy{tag.lower()}.tle"


def parse_tle_text(text: str):
    """Return ``(name, line1, line2)`` from a 2- or 3-line TLE block."""
    lines = [line.rstrip("\r") for line in text.splitlines()]
    lines = [line for line in lines if line.strip()]
    for index in range(len(lines) - 1):
        if lines[index].startswith("1 ") and lines[index + 1].startswith("2 "):
            name = lines[index - 1].strip() if index > 0 and not lines[index - 1].startswith("1 ") else ""
            return name, lines[index].strip(), lines[index + 1].strip()
    return None


def tle_epoch_utc(line1: str):
    """Decode the TLE epoch (columns 19-32) into a UTC datetime."""
    try:
        field = line1[18:32].strip()
        year2 = int(field[:2])
        day = float(field[2:])
        year = 2000 + year2 if year2 < 57 else 1900 + year2
        start = dt.datetime(year, 1, 1, tzinfo=dt.timezone.utc)
        return start + dt.timedelta(days=day - 1.0)
    except Exception:  # noqa: BLE001
        return None


def fetch(norad: str):
    last_error = None
    for template in SOURCES:
        url = template.format(norad=norad)
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "FY2G-Station/1.0"})
            with urllib.request.urlopen(request, timeout=TIMEOUT_SECONDS) as response:
                body = response.read().decode("utf-8", "replace")
            parsed = parse_tle_text(body)
            if parsed:
                return parsed, url
            last_error = f"{url}: no TLE block in response"
        except Exception as exc:  # noqa: BLE001
            last_error = f"{url}: {type(exc).__name__} {exc}"
    raise RuntimeError(last_error or "no source available")


def load_cached():
    """Read the per-satellite cache, falling back to the combined file."""
    cached = {}
    combined = combined_path()
    if not combined.exists():
        return cached
    text = combined.read_text(encoding="utf-8", errors="replace")
    lines = [line.rstrip() for line in text.splitlines() if line.strip()]
    index = 0
    while index < len(lines) - 1:
        if lines[index].startswith("1 ") and lines[index + 1].startswith("2 "):
            name = lines[index - 1].strip() if index > 0 else ""
            for tag, info in SATELLITES.items():
                if info["norad"] in lines[index]:
                    cached[tag] = (name, lines[index], lines[index + 1])
            index += 2
        else:
            index += 1
    return cached


def write_cache(records: dict):
    tle_dir().mkdir(parents=True, exist_ok=True)
    lines = []
    metadata = {"generated_utc": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
                "purpose": "FY-2 S-VISSR scan geometry; TLE must stay current because both "
                           "spacecraft drift in longitude",
                "satellites": {}}
    for tag in ("2G", "2H"):
        if tag not in records:
            continue
        name, line1, line2 = records[tag]
        lines.extend([name or SATELLITES[tag]["name"], line1, line2])
        epoch = tle_epoch_utc(line1)
        cache_path(tag).write_text("\n".join([name or SATELLITES[tag]["name"], line1, line2]) + "\n",
                                   encoding="utf-8")
        metadata["satellites"][tag] = {
            "norad_id": SATELLITES[tag]["norad"],
            "name": name or SATELLITES[tag]["name"],
            "nominal_longitude_deg": SATELLITES[tag]["longitude_deg"],
            "epoch_utc": epoch.isoformat() if epoch else None,
            "age_days": round((dt.datetime.now(dt.timezone.utc) - epoch).total_seconds() / 86400.0, 2)
            if epoch else None,
            "source_file": cache_path(tag).name,
        }
    # Preserve any satellites already present in the combined file but not
    # refreshed in this run.
    known = {SATELLITES[tag]["norad"] for tag in records}
    for tag, info in SATELLITES.items():
        if info["norad"] in known:
            continue
        cached = load_cached().get(tag)
        if cached:
            name, line1, line2 = cached
            lines.extend([name or info["name"], line1, line2])
            epoch = tle_epoch_utc(line1)
            metadata["satellites"][tag] = {
                "norad_id": info["norad"], "name": name or info["name"],
                "nominal_longitude_deg": info["longitude_deg"],
                "epoch_utc": epoch.isoformat() if epoch else None,
                "age_days": round((dt.datetime.now(dt.timezone.utc) - epoch).total_seconds() / 86400.0, 2)
                if epoch else None,
                "source_file": cache_path(tag).name, "preserved": True,
            }
    combined_path().write_text("\n".join(lines) + "\n", encoding="utf-8")
    (tle_dir() / "fy2_tle_metadata.json").write_text(
        json.dumps(metadata, indent=2), encoding="utf-8")
    return metadata


def report(metadata: dict):
    print(f"combined TLE : {combined_path()}")
    for tag, info in metadata.get("satellites", {}).items():
        age = info.get("age_days")
        flag = ""
        if isinstance(age, (int, float)):
            flag = "  <-- REFRESH, older than 7 days" if age > 7 else "  (fresh)"
        print(f"  {tag}: {info['name']} (NORAD {info['norad_id']})  epoch {info.get('epoch_utc')}  "
              f"age {age} d{flag}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--satellite", choices=["2G", "2H", "both"], default="both")
    parser.add_argument("--print", dest="print_only", action="store_true",
                        help="report the cached TLEs without touching the network")
    args = parser.parse_args()

    if args.print_only:
        cached = load_cached()
        metadata = {"satellites": {}}
        for tag, (name, line1, line2) in cached.items():
            epoch = tle_epoch_utc(line1)
            metadata["satellites"][tag] = {
                "norad_id": SATELLITES[tag]["norad"], "name": name,
                "epoch_utc": epoch.isoformat() if epoch else None,
                "age_days": round((dt.datetime.now(dt.timezone.utc) - epoch).total_seconds() / 86400.0, 2)
                if epoch else None,
            }
        report(metadata)
        return 0

    wanted = list(SATELLITES) if args.satellite == "both" else [args.satellite]
    records = {}
    failures = []
    for tag in wanted:
        try:
            parsed, url = fetch(SATELLITES[tag]["norad"])
            records[tag] = parsed
            print(f"{tag}: fetched from {url}")
        except Exception as exc:  # noqa: BLE001
            failures.append(f"{tag}: {exc}")
            print(f"{tag}: fetch failed ({exc})", file=sys.stderr)
    if not records:
        print("no satellite could be refreshed; keeping the existing cache", file=sys.stderr)
        cached = load_cached()
        metadata = {"satellites": {}}
        for tag, (name, line1, line2) in cached.items():
            epoch = tle_epoch_utc(line1)
            metadata["satellites"][tag] = {
                "norad_id": SATELLITES[tag]["norad"], "name": name,
                "epoch_utc": epoch.isoformat() if epoch else None,
                "age_days": round((dt.datetime.now(dt.timezone.utc) - epoch).total_seconds() / 86400.0, 2)
                if epoch else None,
            }
        report(metadata)
        return 1

    metadata = write_cache(records)
    report(metadata)
    if failures:
        print("partial refresh:", "; ".join(failures), file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
