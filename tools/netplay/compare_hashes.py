"""Compare two netplay state-hash logs (see pc_port/netplay/pc_state_hash.h).

Usage: compare_hashes.py A B

Exit 0 if identical. Otherwise prints the first divergent tick and which
sub-hash columns differ there, then exits 1. Also exits 1 (with a reason)
when the files have different line counts or a line fails to parse.
"""

import sys

COLUMNS = ["total", "navi", "piki", "teki", "item", "world", "rng", "mode"]
HEX = set("0123456789abcdefABCDEF")


def parse_line(line, lineno, path):
    parts = line.split()
    if len(parts) != 1 + len(COLUMNS):
        return None, f"{path}:{lineno}: expected {1 + len(COLUMNS)} columns, got {len(parts)}"
    try:
        tick = int(parts[0])
    except ValueError:
        return None, f"{path}:{lineno}: bad tick {parts[0]!r}"
    hashes = []
    for tok in parts[1:]:
        if len(tok) != 16 or any(c not in HEX for c in tok):
            return None, f"{path}:{lineno}: bad hash {tok!r}"
        hashes.append(tok.lower())
    return (tick, hashes), None


def main(argv):
    if len(argv) != 3:
        print("usage: compare_hashes.py A B", file=sys.stderr)
        return 2
    with open(argv[1], "r", errors="replace") as f:
        rows_a = [ln.strip() for ln in f if ln.strip()]
    with open(argv[2], "r", errors="replace") as f:
        rows_b = [ln.strip() for ln in f if ln.strip()]

    n = min(len(rows_a), len(rows_b))
    for i in range(n):
        pa, err = parse_line(rows_a[i], i + 1, argv[1])
        if err is not None:
            print(err)
            return 1
        pb, err = parse_line(rows_b[i], i + 1, argv[2])
        if err is not None:
            print(err)
            return 1
        if pa != pb:
            ta, ha = pa
            tb, hb = pb
            if ta != tb:
                print(f"first divergence at file line {i + 1}: tick {ta} vs {tb}")
            else:
                diff = [c for c, x, y in zip(COLUMNS, ha, hb) if x != y]
                print(f"first divergence at tick {ta}: columns differ: {', '.join(diff)}")
                for c, x, y in zip(COLUMNS, ha, hb):
                    if x != y:
                        print(f"  {c}: {x} vs {y}")
            return 1
    if len(rows_a) != len(rows_b):
        print(f"length mismatch: {argv[1]} has {len(rows_a)} lines, "
              f"{argv[2]} has {len(rows_b)} lines (first {n} identical)")
        return 1
    print(f"identical: {len(rows_a)} ticks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
