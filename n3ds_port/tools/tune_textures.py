#!/usr/bin/env python3
"""Texture quality tuner for the Open Nectar 3DS port (standard library only).

The port shrinks textures while it decodes them, driven by sdmc:/3ds/nectar/nectar.ini, so no game asset is ever rewritten and
nothing needs re-packing. This script helps choose the settings:

  tune_textures.py analyze  log.txt                      what the game loaded, what it cost, and what each setting would save
  tune_textures.py fit      log.txt --budget-kb 4096 -o nectar.ini     smallest quality loss that fits a texture-memory budget
  tune_textures.py preset   low|medium|high|potato -o nectar.ini       ready-made settings
  tune_textures.py preview  texdump/ out/ --max-dim 128 [--point]      shrink the game's TGA dumps to see the loss (no game needed)

Copy the resulting nectar.ini to sdmc:/3ds/nectar/ . Keys (all optional):
  tex_shift     0..3  divide every texture's width/height by 2^n          tex_max_dim  cap on the larger side (power of two)
  tex_min_dim   never shrink a texture below this larger-side size (default 16)
  tex_box       1 average source texels (nicer), 0 point-sample (faster)   tex_force16 1 = no 32-bit textures
  tex_budget_kb texture pool size   tex_off 1 = no textures at all (benchmark)   tex_diag N = time the first N decodes
  fps_cap       60 | 30 (default) | 20 | 15 presented frames per second, vblank aligned; 0 = unlimited
"""
import argparse, os, re, struct, sys

CREATED = re.compile(r"\[tex\] (created|FAILED) #(\d+): (\d+)x(\d+) gc-fmt (\d+) .*?-> pica-fmt (\d+) (\d+)x(\d+)(?: \(shift (\d+)\))? (\d+) KB in ([\d.]+) ms")
DIAG = re.compile(r"\[tex-diag\] #(\d+) phases.*?analyse ([\d.]+) ms \| alloc ([\d.]+) ms \| convert ([\d.]+) ms \| convert AGAIN \(warm\) ([\d.]+) ms \| flush ([\d.]+) ms")
GC_NAMES = {0: "I4", 1: "I8", 2: "IA4", 3: "IA8", 4: "RGB565", 5: "RGB5A3", 6: "RGBA8", 8: "C4", 9: "C8", 10: "C14X2", 14: "CMPR", 64: "RAW"}
PICA_BPP = {0: 4, 1: 2, 2: 2, 3: 2, 4: 2, 5: 1}   # RGBA8, RGB565, RGBA5551, RGBA4, LA8, L8

PRESETS = {
    "high":   dict(tex_shift=0, tex_max_dim=1024, tex_box=1, tex_force16=0),
    "medium": dict(tex_shift=0, tex_max_dim=256, tex_box=1, tex_force16=0),
    "low":    dict(tex_shift=0, tex_max_dim=128, tex_box=1, tex_force16=1),
    "potato": dict(tex_shift=1, tex_max_dim=64, tex_box=0, tex_force16=1, tex_min_dim=16),
}


def pot(n):
    p = 8
    while p < n:
        p <<= 1
    return p


def shift_for(w, h, cap, shift, min_dim=16):
    big = max(w, h)
    s = shift
    if cap:
        c = 16
        while c * 2 <= cap and c < 1024:
            c *= 2
        while (big >> s) > c and s < 3:
            s += 1
    while s > 0 and (big >> s) < min_dim:
        s -= 1
    return s


def parse_log(path):
    tex, diag = [], []
    with open(path, errors="replace") as f:
        for line in f:
            m = CREATED.search(line)
            if m:
                st, idx, w, h, gf, pf, pw, ph, sh, kb, ms = m.groups()
                tex.append(dict(ok=st == "created", idx=int(idx), w=int(w), h=int(h), gc=int(gf), pica=int(pf), pw=int(pw), ph=int(ph),
                                shift=int(sh or 0), kb=int(kb), ms=float(ms)))
                continue
            m = DIAG.search(line)
            if m:
                diag.append(dict(idx=int(m.group(1)), analyse=float(m.group(2)), alloc=float(m.group(3)), convert=float(m.group(4)),
                                 again=float(m.group(5)), flush=float(m.group(6))))
    return tex, diag


def est_kb(t, cap=0, shift=0, force16=False, min_dim=16):
    """Estimated pool KB of texture t (taken from the log, at its ORIGINAL size) under the given settings."""
    s = shift_for(t["w"], t["h"], cap, shift, min_dim)
    sw, sh = (t["w"] + (1 << s) - 1) >> s, (t["h"] + (1 << s) - 1) >> s
    bpp = PICA_BPP.get(t["pica"], 2)
    if force16 and t["pica"] == 0:
        bpp = 2
    return pot(sw) * pot(sh) * bpp / 1024.0


def orig_size(t):
    # the log prints the stored (pot, possibly already shrunk) size; undo a shift from a previous run so estimates start from the original
    return t


def cmd_analyze(a):
    tex, diag = parse_log(a.log)
    if not tex:
        print("no '[tex] created' lines in that log (is it from milestone 1k or later?)")
        return 1
    for t in tex:  # logs from a run that already used tex_shift show the shrunk pot size; w/h stay original
        pass
    total_ms = sum(t["ms"] for t in tex)
    print(f"{len(tex)} textures logged ({sum(t['ok'] for t in tex)} created, {sum(not t['ok'] for t in tex)} failed), decode time {total_ms/1000:.1f} s\n")
    print(f"{'#':>3} {'size':>9} {'gc fmt':>7} {'pica':>5} {'pot size':>9} {'KB':>6} {'ms':>9} {'us/px':>7}")
    for t in tex:
        px = t["w"] * t["h"]
        print(f"{t['idx']:>3} {t['w']:>4}x{t['h']:<4} {GC_NAMES.get(t['gc'], t['gc']):>7} {t['pica']:>5} {t['pw']:>4}x{t['ph']:<4} {t['kb']:>6} {t['ms']:>9.1f} {t['ms']*1000.0/max(px,1):>7.2f}" + ("" if t["ok"] else "  FAILED"))
    slow = [t for t in tex if t["ms"] * 1000.0 / max(t["w"] * t["h"], 1) > 5.0]
    if slow:
        print(f"\n!! {len(slow)} texture(s) decoded at > 5 us per pixel. A healthy decode here is ~0.2-0.5 us/px (800 MHz).")
        print("   If this log is from milestone 1k, that is the TGA debug dump (one fwrite per pixel to the SD card) being counted in the time:")
        print("   only textures <= 512 px that were dumped are slow, larger ones are fine. 1l fixes it (dump is off by default and buffered).")
        print("   With a 1l log, see the [tex-diag] lines below (tex_diag in nectar.ini, default 8 textures).")
    if diag:
        print("\n[tex-diag] first pass vs warm second pass (same work):")
        for d in diag:
            ratio = d["convert"] / d["again"] if d["again"] > 0 else float("inf")
            verdict = "starved/cold on the first pass (contention, not the algorithm)" if ratio > 4 else "decode itself is this slow" if d["convert"] > 500 else "ok"
            print(f"  #{d['idx']}: analyse {d['analyse']:.0f} | alloc {d['alloc']:.0f} | convert {d['convert']:.0f} | again {d['again']:.0f} | flush {d['flush']:.0f} ms -> {verdict}")
    print("\nEstimated pool size by setting (KB):")
    for name, kw in [("as loaded", dict()), ("max_dim 512", dict(cap=512)), ("max_dim 256", dict(cap=256)), ("max_dim 128", dict(cap=128)),
                     ("max_dim 64", dict(cap=64)), ("shift 1", dict(shift=1)), ("shift 1 + force16", dict(shift=1, force16=True)), ("shift 2", dict(shift=2))]:
        print(f"  {name:<20} {sum(est_kb(t, **kw) for t in tex):>9.0f} KB")
    return 0


def write_ini(path, cfg, header):
    lines = ["# Open Nectar 3DS - texture settings (" + header + ")", "# Put this file in sdmc:/3ds/nectar/ . Delete a line to get its default back."]
    lines += [f"{k} = {v}" for k, v in cfg.items()]
    text = "\n".join(lines) + "\n"
    if path == "-":
        sys.stdout.write(text)
    else:
        with open(path, "w") as f:
            f.write(text)
        print(f"wrote {path}")


def cmd_fit(a):
    tex, _ = parse_log(a.log)
    if not tex:
        print("no textures in that log")
        return 1
    tried = []
    for cap in (1024, 512, 256, 128, 64):
        for f16 in (False, True):
            tot = sum(est_kb(t, cap=cap, force16=f16, min_dim=a.min_dim) for t in tex)
            tried.append((cap, f16, tot))
            if tot <= a.budget_kb:
                cfg = dict(tex_max_dim=cap, tex_min_dim=a.min_dim, tex_box=1, tex_force16=int(f16), tex_budget_kb=a.budget_kb)
                print(f"fits: max_dim {cap}{' + force16' if f16 else ''} -> ~{tot:.0f} KB (budget {a.budget_kb} KB)")
                write_ini(a.output, cfg, f"fitted to {a.budget_kb} KB from {os.path.basename(a.log)}")
                return 0
    print(f"even max_dim 64 + force16 needs ~{tried[-1][2]:.0f} KB: raise the budget or use tex_shift=2. (Only {len(tex)} textures are in this log; later scenes load more.)")
    return 2


def cmd_preset(a):
    cfg = dict(PRESETS[a.name]); cfg.setdefault("tex_min_dim", 16)
    write_ini(a.output, cfg, f"preset {a.name}")
    return 0


def read_tga(path):
    with open(path, "rb") as f:
        d = f.read()
    idl, cmap, typ = d[0], d[1], d[2]
    w, h, bpp, desc = struct.unpack_from("<HHBB", d, 12)
    if typ != 2 or cmap or bpp not in (24, 32):
        raise ValueError("only uncompressed 24/32-bit TGA is supported")
    n = bpp // 8
    data = d[18 + idl:18 + idl + w * h * n]
    rows = [data[y * w * n:(y + 1) * w * n] for y in range(h)]
    if not (desc & 0x20):
        rows.reverse()  # store top-down
    px = [[tuple(r[x * n:(x + 1) * n]) + ((255,) if n == 3 else ()) for x in range(w)] for r in rows]
    return w, h, px


def write_tga(path, w, h, px):
    hd = bytearray(18); hd[2] = 2
    struct.pack_into("<HHBB", hd, 12, w, h, 32, 0x28)
    body = bytearray()
    for row in px:
        for p in row:
            body += bytes(p)
    with open(path, "wb") as f:
        f.write(hd + body)


def shrink(w, h, px, s, box):
    n = 1 << s
    sw, sh = (w + n - 1) >> s, (h + n - 1) >> s
    out = []
    for y in range(sh):
        row = []
        for x in range(sw):
            if box:
                acc = [0, 0, 0, 0]; cnt = 0
                for yy in range(y * n, min(y * n + n, h)):
                    for xx in range(x * n, min(x * n + n, w)):
                        p = px[yy][xx]
                        for k in range(4):
                            acc[k] += p[k]
                        cnt += 1
                row.append(tuple(v // cnt for v in acc))
            else:
                row.append(px[min(y * n + n // 2, h - 1)][min(x * n + n // 2, w - 1)])
        out.append(row)
    return sw, sh, out


def cmd_preview(a):
    os.makedirs(a.out_dir, exist_ok=True)
    done = 0
    for name in sorted(os.listdir(a.in_dir)):
        if not name.lower().endswith(".tga"):
            continue
        w, h, px = read_tga(os.path.join(a.in_dir, name))
        s = shift_for(w, h, a.max_dim, a.shift, a.min_dim)
        sw, sh, out = shrink(w, h, px, s, not a.point) if s else (w, h, px)
        write_tga(os.path.join(a.out_dir, name), sw, sh, out)
        print(f"{name}: {w}x{h} -> {sw}x{sh} (shift {s})")
        done += 1
    if not done:
        print("no .tga files found (the game writes the first 24 decoded textures to sdmc:/3ds/nectar/texdump/)")
        return 1
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("analyze"); p.add_argument("log"); p.set_defaults(fn=cmd_analyze)
    p = sub.add_parser("fit"); p.add_argument("log"); p.add_argument("--budget-kb", type=int, required=True); p.add_argument("--min-dim", type=int, default=16)
    p.add_argument("-o", "--output", default="nectar.ini"); p.set_defaults(fn=cmd_fit)
    p = sub.add_parser("preset"); p.add_argument("name", choices=sorted(PRESETS)); p.add_argument("-o", "--output", default="nectar.ini"); p.set_defaults(fn=cmd_preset)
    p = sub.add_parser("preview"); p.add_argument("in_dir"); p.add_argument("out_dir")
    p.add_argument("--max-dim", type=int, default=0); p.add_argument("--shift", type=int, default=0); p.add_argument("--min-dim", type=int, default=16)
    p.add_argument("--point", action="store_true", help="point-sample instead of box average (matches tex_box=0)"); p.set_defaults(fn=cmd_preview)
    a = ap.parse_args()
    sys.exit(a.fn(a))


if __name__ == "__main__":
    main()
