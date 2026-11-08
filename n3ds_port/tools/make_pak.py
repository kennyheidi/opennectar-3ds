#!/usr/bin/env python3
"""Build nectar.pak: the game's small asset files (assets/dataDir, without dataDir/SndData) packed into one file, so the 3DS opens ONE file on the SD card instead of hundreds
(each SD-card fopen costs ~35 ms on the console; a level load opens a few hundred files).

Usage (on the PC, with the SD card or a copy of the nectar folder):
    python make_pak.py  E:/3ds/nectar                 # the folder that holds assets/ (and nectar.ini)
    python make_pak.py  E:/3ds/nectar  --max-file-kb 4096
Writes E:/3ds/nectar/nectar.pak (or -o PATH). Keep the loose assets/ folder: anything not in the pak (big files, cutscene .stx streams,
anything new) is still read from there, and the game falls back to the loose files if the pak is missing or broken.
Re-run the script whenever you change files under assets/ (a stale pak would serve the old copy).
Turn the pak off without deleting it: nectar.ini  pak = 0

Format (little endian), version 1 - must match n3ds_port/platform/n3ds_pak.cpp:
    "NPAK" | u32 version=1 | u32 count | u32 reserved=0
    count x { u64 fnv1a64(key) | u32 offset | u32 size }   sorted by hash
    file data (each file 32-byte aligned)
key = "assets/" + path relative to assets/, lower case, forward slashes (exactly what the game passes to fopen, normalised).
"""
import argparse
import os
import struct
import sys

FNV_OFFSET = 1469598103934665603
FNV_PRIME = 1099511628211
MASK64 = (1 << 64) - 1
DEFAULT_SKIP_EXT = {".stx", ".thp", ".pak"}   # cutscene streams are opened directly (not through DVDOpen) and are huge


def norm_key(path):
    p = path.replace("\\", "/")
    while p.startswith("./"):
        p = p[2:]
    p = p.lstrip("/")
    return p.lower()


def fnv1a64(s):
    h = FNV_OFFSET
    for c in s.encode("utf-8"):
        h ^= c
        h = (h * FNV_PRIME) & MASK64
    return h


def collect(assets_dir, max_bytes, skip_ext):
    """Only assets/dataDir: every path the game opens through its DVD layer starts with dataDir/. dataDir/SndData (sound banks and
    streams) is read by the audio code directly, never through DVDOpen, so it stays out."""
    files, skipped_big, skipped_ext, unreadable = [], 0, 0, []
    data_dir = os.path.join(assets_dir, "dataDir")
    if not os.path.isdir(data_dir):
        sys.exit("error: no dataDir folder in %s" % assets_dir)
    for root, dirs, names in os.walk(data_dir):
        if os.path.normcase(os.path.abspath(root)) == os.path.normcase(os.path.abspath(data_dir)):
            dirs[:] = [d for d in dirs if d.lower() != "snddata"]
        dirs.sort()
        for name in sorted(names):
            if name.startswith("._") or name in (".DS_Store", "Thumbs.db", "desktop.ini"):
                continue
            full = os.path.join(root, name)
            ext = os.path.splitext(name)[1].lower()
            if ext in skip_ext:
                skipped_ext += 1
                continue
            try:
                size = os.path.getsize(full)
                with open(full, "rb"):
                    pass
            except OSError:
                unreadable.append(full)          # odd name / locked / damaged: stays loose
                continue
            if size > max_bytes:
                skipped_big += 1
                continue
            rel = os.path.relpath(full, assets_dir)
            files.append((norm_key("assets/" + rel), full, size))
    return files, skipped_big, skipped_ext, unreadable


def build(base, out_path, max_kb, skip_ext):
    assets_dir = os.path.join(base, "assets")
    if not os.path.isdir(assets_dir):
        if os.path.basename(os.path.normpath(base)).lower() == "assets":
            assets_dir = base
        else:
            sys.exit("error: no assets/ folder in %s (give the folder that contains assets/, e.g. E:/3ds/nectar)" % base)
    files, skipped_big, skipped_ext, unreadable = collect(assets_dir, max_kb * 1024, skip_ext)
    for u in unreadable[:5]:
        print("  skipped (cannot read): %s" % u)
    if len(unreadable) > 5:
        print("  ... and %d more unreadable files" % (len(unreadable) - 5))
    if not files:
        sys.exit("error: no files found under %s" % assets_dir)

    by_hash = {}
    for key, full, size in files:
        h = fnv1a64(key)
        if h in by_hash and by_hash[h] != key:
            sys.exit("error: hash collision between %s and %s - rename one of them" % (by_hash[h], key))
        by_hash[h] = key

    header = 16
    index_size = 16 * len(files)
    data_start = (header + index_size + 31) & ~31
    entries, offset = [], data_start
    for key, full, size in files:            # data in directory order: files loaded together sit together on the card
        entries.append((fnv1a64(key), offset, size, full))
        offset = (offset + size + 31) & ~31
    if offset >= (1 << 32) - 1:
        sys.exit("error: the pak would be %.1f GB; FAT32 and the format stop at 4 GB. Lower --max-file-kb." % (offset / 2**30))

    tmp = out_path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(b"NPAK" + struct.pack("<III", 1, len(entries), 0))
        for h, off, size, _ in sorted(entries, key=lambda e: e[0]):
            f.write(struct.pack("<QII", h, off, size))
        f.write(b"\0" * (data_start - f.tell()))
        for i, (h, off, size, full) in enumerate(entries):
            assert f.tell() == off
            with open(full, "rb") as src:
                data = src.read()
            if len(data) != size:
                sys.exit("error: %s changed size while packing" % full)
            f.write(data)
            f.write(b"\0" * (((off + size + 31) & ~31) - (off + size)))
            if (i + 1) % 500 == 0:
                print("  %d / %d files" % (i + 1, len(entries)))
    os.replace(tmp, out_path)
    print("wrote %s: %d files from assets/dataDir, %.1f MB (left loose: %d over %d KB, %d by extension %s, %d unreadable; SndData not packed)"
          % (out_path, len(entries), offset / 2**20, skipped_big, max_kb, skipped_ext, ",".join(sorted(skip_ext)), len(unreadable)))
    print("copy it next to nectar.ini (sdmc:/3ds/nectar/nectar.pak) and keep the assets/ folder.")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", help="the nectar folder that contains assets/ (or the assets/ folder itself)")
    ap.add_argument("-o", "--output", help="output file (default: <folder>/nectar.pak)")
    ap.add_argument("--max-file-kb", type=int, default=2048,
                    help="only pack files up to this size (default 2048). Opening is what is slow; big files are dominated by reading anyway")
    ap.add_argument("--include-ext", default="", help="comma list of extensions to pack even though they are skipped by default (e.g. .thp)")
    a = ap.parse_args()
    skip = set(DEFAULT_SKIP_EXT) - {e.strip().lower() for e in a.include_ext.split(",") if e.strip()}
    base = a.folder
    out = a.output or os.path.join(base if os.path.basename(os.path.normpath(base)).lower() != "assets" else os.path.dirname(os.path.normpath(base)), "nectar.pak")
    build(base, out, a.max_file_kb, skip)


if __name__ == "__main__":
    main()
