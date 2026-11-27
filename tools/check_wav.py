#!/usr/bin/env python3
import argparse
import wave

p = argparse.ArgumentParser(description="Check FY-2 WAV test file layout")
p.add_argument("wav")
p.add_argument("--fraction", type=float, default=0.5)
a = p.parse_args()

with wave.open(a.wav, "rb") as w:
    ch = w.getnchannels()
    width = w.getsampwidth()
    sr = w.getframerate()
    frames = w.getnframes()
    dur = frames / sr if sr else 0.0
    f = max(0.01, min(1.0, a.fraction))
    take = int(frames * f)

print(f"channels={ch}")
print(f"sample_width_bytes={width}")
print(f"sample_rate={sr}")
print(f"frames={frames}")
print(f"duration_s={dur:.6f}")
print(f"selected_frames={take}")
print(f"selected_duration_s={take/sr:.6f}")

if ch != 2 or width != 2:
    raise SystemExit("This GUI WAV mode expects stereo 16-bit PCM.")
