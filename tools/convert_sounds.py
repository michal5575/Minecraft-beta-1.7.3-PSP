#!/usr/bin/env python3
"""Converts the Beta 1.7.3 sound resources into PSP-friendly IMA ADPCM.

Usage: convert_sounds.py <resources dir> <output dir>

Writes <out>/sounds.pak (all effects, mono 22050 Hz, loaded into RAM) and
<out>/music/*.wav, <out>/records/*.wav (stereo 22050 Hz IMA ADPCM WAV, streamed).
Requires ffmpeg.
"""
import os
import re
import struct
import subprocess
import sys
import tempfile

# Sound groups the game plays (Beta 1.7.3 names: path with '/' -> '.', trailing digits removed).
EFFECT_DIRS = ["step", "random", "mob", "ambient", "liquid", "fire", "portal", "note", "tile"]
SKIP = re.compile(
    r"^(mob\.(blaze|cat|endermen|irongolem|magmacube|silverfish|zombie\.(metal|wood|woodbreak))"
    r"|mob\.ghast\.affectionate|random\.(bowhit|breath|burp|drink|eat|levelup|orb|old_explode|chest|wood)"
    r"|liquid\.(lava|lavapop|splash)|damage\.)"
)

RATE = 22050
# Long atmospheric effects are stored at half rate to save RAM.
LOW_RATE = re.compile(r"^(ambient\.cave|ambient\.weather\.thunder|portal\.)")


def ffmpeg_ima(src, channels, rate=RATE):
    tmp = tempfile.NamedTemporaryFile(suffix=".wav", delete=False)
    tmp.close()
    try:
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", src, "-ac", str(channels), "-ar", str(rate), "-c:a",
                        "adpcm_ima_wav", tmp.name], check=True)
        with open(tmp.name, "rb") as f:
            return f.read()
    finally:
        os.unlink(tmp.name)


def parse_wav(data):
    i = 12
    fmt = None
    samples = None
    payload = None
    while i + 8 <= len(data):
        cid = data[i:i + 4]
        size = struct.unpack_from("<I", data, i + 4)[0]
        body = data[i + 8:i + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", body)
        elif cid == b"fact":
            samples = struct.unpack_from("<I", body)[0]
        elif cid == b"data":
            payload = body
        i += 8 + size + (size & 1)
    tag, channels, rate, _, block_align, _ = fmt
    assert tag == 0x11, "not IMA ADPCM"
    return channels, rate, block_align, samples, payload


def group_name(rel):
    name = os.path.splitext(rel)[0].replace(os.sep, ".").replace("/", ".")
    return re.sub(r"\d+$", "", name)


def build_effects(res, out):
    base = os.path.join(res, "newsound")
    entries = []
    for d in EFFECT_DIRS:
        for root, _, files in os.walk(os.path.join(base, d)):
            for f in files:
                if not f.endswith(".ogg") or " " in f:
                    continue
                rel = os.path.relpath(os.path.join(root, f), base)
                name = group_name(rel)
                if SKIP.match(name):
                    continue
                entries.append((name, os.path.join(root, f)))
    entries.sort()
    blobs = []
    for name, path in entries:
        want = RATE // 2 if LOW_RATE.match(name) else RATE
        ch, rate, align, samples, payload = parse_wav(ffmpeg_ima(path, 1, want))
        assert ch == 1 and rate == want and len(name) < 32
        blobs.append((name, samples, rate, align, payload))
    header_size = 8 + len(blobs) * 48
    offset = header_size
    with open(os.path.join(out, "sounds.pak"), "wb") as f:
        f.write(b"MCSP" + struct.pack("<I", len(blobs)))
        for name, samples, rate, align, payload in blobs:
            f.write(name.encode().ljust(32, b"\0"))
            f.write(struct.pack("<IIIHH", offset, len(payload), samples, rate, align))
            offset += len(payload)
        for blob in blobs:
            f.write(blob[4])
    groups = sorted({b[0] for b in blobs})
    print("sounds.pak: %d sounds in %d groups, %d KB" % (len(blobs), len(groups), offset // 1024))


def build_streams(res, out):
    jobs = [("music", os.path.join(res, "music")), ("music", os.path.join(res, "newmusic")),
            ("records", os.path.join(res, "streaming"))]
    total = 0
    for sub, src in jobs:
        os.makedirs(os.path.join(out, sub), exist_ok=True)
        for f in sorted(os.listdir(src)):
            if not f.endswith(".ogg"):
                continue
            dst = os.path.join(out, sub, f[:-4] + ".wav")
            if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(os.path.join(src, f)):
                data = ffmpeg_ima(os.path.join(src, f), 2)
                parse_wav(data)
                with open(dst, "wb") as o:
                    o.write(data)
            total += os.path.getsize(dst)
    print("streams: %d KB" % (total // 1024))


def main():
    res, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    build_effects(res, out)
    build_streams(res, out)


if __name__ == "__main__":
    main()
