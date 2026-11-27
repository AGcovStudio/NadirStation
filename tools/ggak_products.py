"""Export reproducible GGAK N4 quicklook products from a saved .cadu capture.

The fields and instrument assignments are empirical. No absolute calibration is
applied; read the notes in src/ggak.cpp before interpreting these products.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import csv
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import statistics
from PIL import Image, ImageDraw, ImageFont

FRAME_BYTES = 224
ASM = bytes.fromhex("1acffc1d")
EPOCH_UNIX = 946684800 - 3 * 3600

# (filename stem, provisional instrument, confidence, record bytes, fields).
# Field tuples are (CSV name, offset, format). Offsets start at zero within a record.
LAYOUTS = {
    12: ("vuss_e_candidate", "VUSS-E? UV", "candidate", 12,
         [("byte3", 3, "u8"), ("byte5", 5, "u8")]),
    28: ("dir_e_xray", "DIR-E X-ray", "strong", 12,
         [("xray_response_byte2", 2, "u8"), ("byte3", 3, "u8")]),
    44: ("skif6_candidate", "SKIF-6? particles", "candidate", 69,
         [("byte5", 5, "u8"), ("word6", 6, "u16"), ("word8", 8, "u16"),
          ("byte11", 11, "u8"), ("word48", 48, "u16")]),
    60: ("skl_e_candidate", "SKL-E? particles", "candidate", 69,
         [("word8", 8, "u16"), ("word10", 10, "u16"), ("word12", 12, "u16"),
          ("word14", 14, "u16"), ("byte41", 41, "u8"), ("word48", 48, "u16")]),
    64: ("gals_e", "GALS-E? cosmic rays", "strong", 19,
         [("raw_word3", 3, "u16"), ("raw_word5", 5, "u16"),
          ("raw_word7", 7, "u16")]),
    80: ("fm_e", "FM-E? magnetometer", "strong", 104,
         [("axis1_raw", 4, "s24"), ("axis2_raw", 7, "s24"),
          ("axis3_raw", 10, "s24")]),
    112: ("isp2m_candidate", "ISP-2M? solar constant", "candidate", 15,
          [("word0", 0, "u16"), ("word2", 2, "u16"),
           ("word4", 4, "u16"), ("word6", 6, "u16")]),
}
INTERVALS = {12: 17, 28: 17, 44: 3, 60: 3, 64: 11, 80: 2, 112: 13}
COLORS = ("#d8ad70", "#9eae94", "#c1c3c0", "#b89ca0", "#c5bda2", "#9fa9a3")


def iso_minute(minute: int) -> str:
    return datetime.fromtimestamp(minute * 60, timezone.utc).strftime("%Y-%m-%dT%H:%M:00Z")


def value(record: bytes, offset: int, kind: str) -> int:
    if kind == "u8":
        return record[offset]
    if kind == "u16":
        return int.from_bytes(record[offset:offset + 2], "big")
    n = int.from_bytes(record[offset:offset + 3], "big")
    return n - 0x1000000 if n & 0x800000 else n


def percentile(values: list[int], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return float("nan")
    pos = fraction * (len(ordered) - 1)
    lo = int(pos)
    return ordered[lo] + (ordered[min(lo + 1, len(ordered) - 1)] - ordered[lo]) * (pos - lo)


def csv_write(path: Path, headers: list[str], rows: list[list[object]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(headers)
        writer.writerows(rows)


def frame_records(data: bytes):
    counts = Counter()
    record_counts = Counter()
    minute_frames = defaultdict(Counter)
    minute_values = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    observed_minutes = set()
    for offset in range(0, len(data) - FRAME_BYTES + 1, FRAME_BYTES):
        frame = data[offset:offset + FRAME_BYTES]
        ident = frame[4]
        if frame[:4] != ASM:
            counts["bad_asm"] += 1
            continue
        if sum(frame[:222]) & 0xFFFF != int.from_bytes(frame[222:224], "big"):
            counts["bad_checksum"] += 1
            continue
        counts[f"id_{ident}"] += 1
        if ident == 119:
            counts["filler"] += 1
            continue  # Filler time is 0x33333333 and is not a real observation.
        if ident not in LAYOUTS:
            counts["unknown_valid"] += 1
            continue
        timestamp = EPOCH_UNIX + int.from_bytes(frame[9:13], "big")
        frame_minute = timestamp // 60
        observed_minutes.add(frame_minute)
        minute_frames[frame_minute][ident] += 1
        _, _, _, stride, fields = LAYOUTS[ident]
        for index in range(209 // stride):
            record = frame[13 + index * stride:13 + (index + 1) * stride]
            if record[:4] == b"\x33" * 4:
                break
            if ident == 80 and record[0] != 0xA8:
                counts["id80_non_a8_record"] += 1
                continue
            rec_minute = (timestamp + index) // 60
            observed_minutes.add(rec_minute)
            record_counts[ident] += 1
            bucket = minute_values[ident][rec_minute]
            for name, field_offset, kind in fields:
                bucket[name].append(value(record, field_offset, kind))
    return counts, record_counts, minute_frames, minute_values, observed_minutes


def plot_png(path: Path, title: str, subtitle: str, panels: list[tuple[str, list[tuple[str, dict[int, float]]]]],
             minutes: list[int]) -> None:
    width, panel_h = 1600, 260
    height = 110 + panel_h * len(panels) + 52
    image = Image.new("RGB", (width, height), "#191b1e")
    draw = ImageDraw.Draw(image)
    font_path = Path("C:/Windows/Fonts/arial.ttf")
    font_bold_path = Path("C:/Windows/Fonts/arialbd.ttf")
    def font(size: int, bold: bool = False):
        p = font_bold_path if bold else font_path
        return ImageFont.truetype(str(p), size) if p.exists() else ImageFont.load_default()
    draw.text((50, 24), title, fill="#e9ebec", font=font(29, True))
    draw.text((50, 64), subtitle, fill="#a8acae", font=font(17))
    if not minutes:
        image.save(path)
        return
    first, last = minutes[0], minutes[-1]
    span = max(1, last - first)
    for panel_index, (label, lines) in enumerate(panels):
        top = 110 + panel_index * panel_h
        left, right, bottom = 88, width - 56, top + 180
        draw.rectangle((42, top - 8, width - 35, top + panel_h - 14), fill="#242629",
                       outline="#414448", width=1)
        draw.text((65, top + 2), label, fill="#e1e3e4", font=font(19, True))
        values = [v for _, line in lines for v in line.values()]
        if not values:
            draw.text((100, top + 90), "No data", fill="#a8acae", font=font(16))
            continue
        lo, hi = min(values), max(values)
        if lo == hi:
            lo, hi = lo - 1, hi + 1
        margin = 0.06 * (hi - lo)
        lo -= margin
        hi += margin
        for grid in range(4):
            y = top + 35 + (bottom - top - 35) * grid / 3
            draw.line((left, y, right, y), fill="#3d4145", width=1)
            draw.text((48, y - 8), f"{hi - (hi - lo) * grid / 3:.4g}",
                      fill="#a0a5a7", font=font(12))
        for line_index, (name, line) in enumerate(lines):
            color = COLORS[line_index % len(COLORS)]
            for run in _runs([(left + (m - first) * (right - left) / span,
                               bottom - (v - lo) * (bottom - top - 35) / (hi - lo))
                              if m in line else None for m in minutes for v in [line.get(m, 0)]]):
                if len(run) > 1:
                    draw.line(run, fill=color, width=3)
                elif run:
                    draw.ellipse((run[0][0] - 2, run[0][1] - 2, run[0][0] + 2, run[0][1] + 2), fill=color)
            draw.text((left + line_index * 225, bottom + 13), name, fill=color, font=font(14))
        for frac in (0, 0.25, 0.5, 0.75, 1):
            m = int(first + frac * span)
            draw.text((left + frac * (right - left) - 20, bottom + 42),
                      datetime.fromtimestamp(m * 60, timezone.utc).strftime("%H:%M"),
                      fill="#a0a5a7", font=font(12))
    draw.text((50, height - 32), "UTC | Raw telemetry proxies; instrument names and scales are provisional",
              fill="#a8acae", font=font(14))
    image.save(path)


def _runs(points):
    run = []
    for point in points:
        if point is None:
            if run:
                yield run
            run = []
        else:
            run.append(point)
    if run:
        yield run


def export(input_path: Path, output_dir: Path) -> dict:
    data = input_path.read_bytes()
    output_dir.mkdir(parents=True, exist_ok=True)
    counts, record_counts, minute_frames, minute_values, observed = frame_records(data)
    if not observed:
        raise ValueError("No valid N4 instrument frames found")
    minutes = list(range(min(observed), max(observed) + 1))
    coverage_headers = ["time_utc", "valid_instrument_frames", "valid_filler_frames_not_timed"]
    coverage_headers += [f"id{ident}_frames" for ident in LAYOUTS]
    coverage_headers += [f"id{ident}_expected_frames_approx" for ident in LAYOUTS]
    coverage_rows = []
    for m in minutes:
        by_id = minute_frames[m]
        row = [iso_minute(m), sum(by_id.values()), ""]
        row += [by_id[ident] for ident in LAYOUTS]
        row += [round(60 / INTERVALS[ident], 2) for ident in LAYOUTS]
        coverage_rows.append(row)
    csv_write(output_dir / "frame_coverage_1min.csv", coverage_headers, coverage_rows)

    primary = {}
    all_series = {}
    manifest_products = []
    for ident, (stem, instrument, confidence, stride, fields) in LAYOUTS.items():
        names = [f[0] for f in fields]
        headers = ["time_utc", "records"]
        for name in names:
            headers += [f"{name}_median", f"{name}_p10", f"{name}_p90"]
        rows = []
        series = {name: {} for name in names}
        for m in minutes:
            bucket = minute_values[ident].get(m, {})
            n = len(bucket.get(names[0], []))
            row = [iso_minute(m), n]
            for name in names:
                vals = bucket.get(name, [])
                if vals:
                    mid = statistics.median(vals)
                    series[name][m] = mid
                    row += [mid, round(percentile(vals, 0.1), 3), round(percentile(vals, 0.9), 3)]
                else:
                    row += ["", "", ""]
            rows.append(row)
        csv_write(output_dir / f"{stem}_1min.csv", headers, rows)
        all_series[ident] = series
        primary[ident] = series[names[0]]
        # One panel per channel keeps raw channels in their own numeric range.
        panels = [(name + " (raw counts)", [(name, series[name])]) for name in names]
        plot_png(output_dir / f"{stem}_quicklook.png", instrument,
                 f"{len(series[names[0]])} minutes with data | {record_counts[ident]} records | {confidence} mapping",
                 panels, minutes)
        manifest_products.append({"id": ident, "file": f"{stem}_1min.csv",
                                  "quicklook": f"{stem}_quicklook.png", "instrument": instrument,
                                  "confidence": confidence, "fields": names,
                                  "records": record_counts[ident], "record_bytes": stride})

    overview_lines = [(LAYOUTS[ident][1], primary[ident]) for ident in LAYOUTS]
    plot_png(output_dir / "ggak_overview.png", "GGAK N4 | instrument overview",
             f"{iso_minute(minutes[0])} to {iso_minute(minutes[-1])} | {len(data) // FRAME_BYTES:,} frames",
             [(name, [(name, series)]) for name, series in overview_lines], minutes)

    xray_baseline = statistics.median(primary[28].values()) if primary[28] else 0
    csv_write(output_dir / "dir_e_xray_relative_1min.csv",
              ["time_utc", "xray_raw_byte2_median", "capture_median_raw",
               "relative_to_capture_median", "percent_change"],
              [[iso_minute(m), primary[28].get(m, ""), xray_baseline,
                round(primary[28][m] / xray_baseline, 5) if m in primary[28] and xray_baseline else "",
                round(100 * (primary[28][m] / xray_baseline - 1), 2)
                if m in primary[28] and xray_baseline else ""] for m in minutes])

    gals = all_series[64]
    csv_write(output_dir / "gals_e_count_proxy_1min.csv",
              ["time_utc", "channel1_halfword", "channel2_halfword", "channel3_halfword",
               "sum_halfword", "conversion_status"],
              [[iso_minute(m)] +
               ([round(gals[name][m] / 2, 3) for name in ("raw_word3", "raw_word5", "raw_word7")] +
                [round(sum(gals[name][m] for name in ("raw_word3", "raw_word5", "raw_word7")) / 2, 3),
                 "estimated_from_even_words"]
                if all(m in gals[name] for name in ("raw_word3", "raw_word5", "raw_word7"))
                else ["", "", "", "", ""]) for m in minutes])

    fm = all_series[80]
    axes = ("axis1_raw", "axis2_raw", "axis3_raw")
    fm_early = [m for m in minutes[:30] if all(m in fm[name] for name in axes)]
    fm_reference = {name: statistics.median(fm[name][m] for m in fm_early)
                    if fm_early else (statistics.median(fm[name].values()) if fm[name] else 0)
                    for name in axes}
    field_rows = []
    for m in minutes:
        if all(m in fm[name] for name in axes):
            delta = [fm[name][m] - fm_reference[name] for name in axes]
            field_rows.append([iso_minute(m), *[round(v, 3) for v in delta],
                               round(sum(v * v for v in delta) ** 0.5, 3)])
        else:
            field_rows.append([iso_minute(m), "", "", "", ""])
    csv_write(output_dir / "fm_e_relative_vector_1min.csv",
              ["time_utc", "axis1_delta_raw", "axis2_delta_raw", "axis3_delta_raw",
               "vector_delta_norm_raw"], field_rows)

    combined_headers = ["time_utc", "xray_raw_byte2", "uv_candidate_byte3",
                        "skif6_candidate_byte5", "skl_e_candidate_word8",
                        "gals_e_raw_word3", "fm_e_raw_axis1", "isp2m_candidate_word0"]
    csv_write(output_dir / "ggak_selected_channels_1min.csv", combined_headers,
              [[iso_minute(m)] + [primary[ident].get(m, "") for ident in LAYOUTS] for m in minutes])

    # A local raw-response peak catalogue. No GOES flare class or absolute flux is inferred.
    xray = primary[28]
    baseline = xray_baseline
    mad = statistics.median(abs(v - baseline) for v in xray.values()) if xray else 0
    threshold = baseline + max(12, 4 * mad)
    peak_rows = []
    active = []
    for m in minutes + [minutes[-1] + 1]:
        if m in xray and xray[m] >= threshold:
            active.append(m)
        elif active:
            peak = max(active, key=lambda minute: xray[minute])
            if len(active) >= 2 and xray[peak] >= baseline + max(20, 6 * mad):
                peak_rows.append([iso_minute(active[0]), iso_minute(peak), iso_minute(active[-1]),
                                  xray[peak], round(xray[peak] - baseline, 3),
                                  round(baseline, 3), round(threshold, 3), len(active),
                                  int(active[0] == minutes[0])])
            active = []
    csv_write(output_dir / "dir_e_xray_event_candidates.csv",
              ["start_utc", "peak_utc", "end_utc", "peak_raw_byte2_median",
               "peak_above_baseline", "capture_baseline", "threshold", "minutes_above_threshold",
               "left_censored_at_capture_start"], peak_rows)

    manifest = {
        "product_version": 1,
        "source_file": str(input_path.resolve()),
        "source_sha256": hashlib.sha256(data).hexdigest(),
        "source_bytes": len(data),
        "whole_frames": len(data) // FRAME_BYTES,
        "trailing_bytes": len(data) % FRAME_BYTES,
        "utc_start": iso_minute(minutes[0]),
        "utc_end": iso_minute(minutes[-1]),
        "frame_counts": dict(sorted(counts.items())),
        "products": manifest_products,
        "other_products": ["frame_coverage_1min.csv", "dir_e_xray_event_candidates.csv",
                           "dir_e_xray_relative_1min.csv", "gals_e_count_proxy_1min.csv",
                           "fm_e_relative_vector_1min.csv", "ggak_selected_channels_1min.csv",
                           "ggak_overview.png"],
        "fm_e_reference_raw": fm_reference,
        "xray_event_threshold_raw": threshold,
        "warnings": [
            "Instrument IDs are inferred from frame structure; ID 28 also matches the independent GOES X-ray curve.",
            "All exported quantities are raw telemetry counts, not calibrated physical units.",
            "Record timestamps assume one record per second as observed in this capture.",
            "ID 80 non-0xA8 records are excluded from axis products and counted in frame_counts.",
            "Filler ID 119 has no usable observation time and is excluded from time products.",
        ],
    }
    (output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                                               encoding="utf-8")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="GGAK .cadu file")
    parser.add_argument("output", type=Path, help="directory for CSV and PNG products")
    args = parser.parse_args()
    result = export(args.input, args.output)
    print(f"GGAK products: {len(result['products'])} instruments, "
          f"{result['whole_frames']} frames, {result['utc_start']} to {result['utc_end']}")
    print(args.output.resolve())


if __name__ == "__main__":
    main()
