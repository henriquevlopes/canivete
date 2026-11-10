"""Checks for recordings made by tools/test_record.ps1 (needs ffmpeg/ffprobe on PATH, numpy, Pillow).

  check_record.py duration <file.mp4>           -> "video=<s> audio=<s>"
  check_record.py sync <file.mp4>               -> flash / beep onsets and the largest gap between them
  check_record.py gif <file.gif>                -> "frames=<n> duration=<s>"
  check_record.py frame <file.mp4> <t> <out.png> -> saves the frame at t s and counts ring / pill pixels

The test picture's top-right corner turns white for 100 ms each whole second of real time, and the
test sound beeps (1 kHz, loud) at the same moments: in a good recording the two line up.
"""
import json
import subprocess
import sys

import numpy as np


def probe(path, *args):
    out = subprocess.run(["ffprobe", "-v", "error", *args, "-of", "json", path], capture_output=True, text=True, check=True)
    return json.loads(out.stdout)


def duration(path):
    j = probe(path, "-show_entries", "stream=codec_type,duration")
    d = {s["codec_type"]: float(s.get("duration", 0)) for s in j["streams"]}
    print(f"video={d.get('video', 0):.3f} audio={d.get('audio', 0):.3f}")


def video_onsets(path):
    j = probe(path, "-select_streams", "v", "-show_entries", "frame=pts_time")
    pts = [float(f["pts_time"]) for f in j["frames"]]
    w = int(probe(path, "-select_streams", "v", "-show_entries", "stream=width")["streams"][0]["width"])
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-vf", f"crop=12:12:{w - 22}:10,scale=1:1",
                          "-f", "rawvideo", "-pix_fmt", "gray", "-"], capture_output=True, check=True).stdout
    lum = np.frombuffer(raw, np.uint8)
    n = min(len(lum), len(pts))
    on = [pts[i] for i in range(1, n) if lum[i] > 128 and lum[i - 1] <= 128]
    if n and lum[0] > 128:
        on.insert(0, pts[0])
    return on


def audio_onsets(path):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", "48000", "-f", "s16le", "-"],
                         capture_output=True, check=True).stdout
    a = np.frombuffer(raw, np.int16).astype(np.float32) / 32768
    win = 96  # 2 ms
    k = len(a) // win
    rms = np.sqrt((a[: k * win].reshape(k, win) ** 2).mean(axis=1))
    loud = rms > 0.25  # beep 0.6 peak (rms 0.42), quiet tone 0.1 (rms 0.07)
    on = [i * win / 48000 for i in range(1, k) if loud[i] and not loud[i - 1]]
    if k and loud[0]:
        on.insert(0, 0.0)
    return on


def sync(path):
    v, a = video_onsets(path), audio_onsets(path)
    print("flash at", " ".join(f"{t:.3f}" for t in v))
    print("beep at ", " ".join(f"{t:.3f}" for t in a))
    gaps = [min(abs(t - x) for x in a) for t in v] if a else []
    worst = max(gaps) if gaps else 99
    print(f"pairs={len(gaps)} worst={worst * 1000:.0f}ms")


def gif(path):
    from PIL import Image
    im = Image.open(path)
    total, n = 0, 0
    try:
        while True:
            total += im.info.get("duration", 0)
            n += 1
            im.seek(im.tell() + 1)
    except EOFError:
        pass
    print(f"frames={n} duration={total / 1000:.3f}")


def frame(path, t, out):
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-ss", t, "-i", path, "-frames:v", "1", out], check=True)
    from PIL import Image
    px = np.asarray(Image.open(out).convert("RGB")).astype(int)
    r, g, b = px[..., 0], px[..., 1], px[..., 2]
    blue = ((b > 200) & (r < 110) & (g > 120) & (g < 200)).sum()      # right-click ring
    h = px.shape[0]
    bottom = px[int(h * 0.6):]
    dark = ((bottom.max(axis=2) < 70)).sum()                          # pill background
    white = ((bottom.min(axis=2) > 225)).sum()                        # pill text
    print(f"blue={blue} pill_dark={dark} pill_white={white}")


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "duration":
        duration(sys.argv[2])
    elif cmd == "sync":
        sync(sys.argv[2])
    elif cmd == "gif":
        gif(sys.argv[2])
    elif cmd == "frame":
        frame(sys.argv[2], sys.argv[3], sys.argv[4])
