#!/usr/bin/env python3
"""Turn a captured S-1 serial log into the verdict for docs/spikes/s1.md.

Usage:  python3 tools/s1_report.py run.log

The pass criterion (design.md §13) has two halves and they fail differently:

  "within 1%"        — an endpoint check. A run can pass this while bleeding
                       slowly, because 500 cycles of a small leak may still land
                       inside 1% of a 2 MB pool.
  "no downward trend" — a shape check. This is the half that catches the bleed,
                       so the slope of a least-squares fit over the whole series
                       is reported in bytes per cycle, not just the endpoints.

Fragmentation is reported too: a heap that returns to its starting free size but
fragments monotonically passes at cycle 500 and fails in service.
"""

import sys

# §13: free heap must return to its starting value within this fraction.
TOLERANCE_PCT = 1.0

# A slope this small over 500 cycles is a rounding artefact, not a trend. Stated
# as a threshold rather than "slope == 0" so the check survives a noisy series.
SLOPE_EPSILON_BYTES_PER_CYCLE = 0.5


def parse(path):
    rows, header = [], None
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if line.startswith("tag,cycle,"):
            header = line.split(",")
        elif line.startswith("S1,") and header:
            values = line.split(",")
            if len(values) == len(header):
                rows.append(dict(zip(header, values)))
    if header is None:
        sys.exit(f"{path}: no CSV header found — is this an S-1 log?")
    return rows


def linear_slope(xs, ys):
    """Least-squares slope of ys against xs, in y-units per x-unit."""
    n = len(xs)
    mean_x, mean_y = sum(xs) / n, sum(ys) / n
    denom = sum((x - mean_x) ** 2 for x in xs)
    if denom == 0:
        return 0.0
    return sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys)) / denom


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    rows = parse(sys.argv[1])
    if not rows:
        sys.exit("no measured (S1) cycles in log")

    cycles = [int(r["cycle"]) for r in rows]
    free = [int(r["free"]) for r in rows]
    built_free = [int(r["built_free"]) for r in rows]
    frag = [int(r["frag_pct"]) for r in rows]
    tiles = [int(r["tiles"]) for r in rows]
    int_free = [int(r["int_free"]) for r in rows]
    psram_free = [int(r["psram_free"]) for r in rows]

    baseline, final = free[0], free[-1]
    delta = final - baseline
    delta_pct = 100.0 * delta / baseline
    slope = linear_slope(cycles, free)

    # Peak tree cost: how much the standing tree occupied, per cycle.
    tree_cost = [f - b for f, b in zip(free, built_free)]

    print(f"cycles measured      : {len(rows)}  (cycle {cycles[0]}..{cycles[-1]})")
    print(f"tiles per cycle      : {min(tiles)}..{max(tiles)}")
    print()
    print(f"LVGL free @ first    : {baseline:,} B")
    print(f"LVGL free @ last     : {final:,} B")
    print(f"delta                : {delta:+,} B  ({delta_pct:+.4f} %)")
    print(f"min / max over run   : {min(free):,} / {max(free):,} B")
    print(f"trend                : {slope:+.4f} B/cycle "
          f"({slope * len(rows):+,.0f} B over the run)")
    print()
    print(f"tree cost (built)    : {min(tree_cost):,}..{max(tree_cost):,} B")
    print(f"fragmentation        : {min(frag)}..{max(frag)} %")
    print(f"ESP internal free    : {min(int_free):,}..{max(int_free):,} B")
    print(f"ESP PSRAM free       : {min(psram_free):,}..{max(psram_free):,} B")
    print()

    # The tree has to have cost something, or a flat series proves nothing about
    # rebuilds — it just proves nothing happened.
    if max(tree_cost) <= 0:
        print("VERDICT: INVALID — the built tree allocated nothing; the harness "
              "is not measuring a rebuild")
        return 2

    within_tolerance = abs(delta_pct) <= TOLERANCE_PCT
    no_downward_trend = slope >= -SLOPE_EPSILON_BYTES_PER_CYCLE

    print(f"within {TOLERANCE_PCT} %          : {'yes' if within_tolerance else 'NO'}")
    print(f"no downward trend    : {'yes' if no_downward_trend else 'NO'}")
    print()
    verdict = "PASS" if within_tolerance and no_downward_trend else "FAIL"
    print(f"VERDICT: {verdict}")
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
