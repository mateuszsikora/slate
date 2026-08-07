#!/usr/bin/env python3
"""Side-by-side comparison of several S-2 runs.

    python3 tools/s2_compare.py /tmp/s2_a.log /tmp/s2_b.log ...

Emits the Markdown table that goes into docs/spikes/s2.md, so the numbers in the
document are transcribed by a script rather than by hand. The label of each
column is derived from the run's own S2CFG line, not from the file name — a log
that was captured from the wrong build therefore labels itself correctly.
"""
import sys

from s2_report import parse, pct


def label(cfg):
    bits = [f"{cfg.get('num_fbs', '?')} FB"]
    bits.append("bounce" if cfg.get("bounce_px", "0") != "0" else "no bounce")
    dl = cfg.get("draw_lines", "0")
    if dl != "0":
        bits.append(f"{dl} lines")
    bits.append(f"{cfg.get('tiles_built', '?')} tiles")
    return ", ".join(bits)


def steady(frames, switches):
    """Frames outside the page-switch windows — the same rule as s2_report."""
    switch_idx = {s["frame_index"] for s in switches}
    return [f for f in frames if f["idx"] not in switch_idx and f["idx"] > 0]


def main(paths):
    cols = []
    for p in paths:
        cfg, frames, switches = parse(p)
        if not frames:
            sys.exit(f"{p}: no frames")
        st = steady(frames, switches)
        span = (st[-1]["start"] - st[0]["start"]) / 1e6
        rf = [f["render"] - f.get("wait", 0) for f in st]  # see s2_report.main
        torn = sum(1 for f in frames if f["tear"])
        radio = "—"
        for line in open(p, encoding="utf-8", errors="replace"):
            if line.startswith("S2RADIO,"):
                v = line.strip().split(",")
                radio = "idle" if v[2] == "0" else f"{int(v[2])} frames"
        cols.append({
            "label": label(cfg),
            "fps": f"{len(st) / span:.1f}",
            "p50": f"{pct(rf, 50):,}",
            "p95": f"{pct(rf, 95):,}",
            "max": f"{max(rf):,}",
            "tear": f"{torn / span:.1f}",
            "switch": f"{pct([s['destroy'] + s['build'] + s['first_frame'] for s in switches], 50) / 1000:.0f}" if switches else "—",
            "int_free": f"{int(cfg.get('int_free', 0)):,}",
            "psram_free": f"{int(cfg.get('psram_free', 0)):,}",
            "radio": radio,
        })

    rows = [
        ("effective fps", "fps"),
        ("frame work p50 (us)", "p50"),
        ("frame work p95 (us)", "p95"),
        ("frame work max (us)", "max"),
        ("torn frames / s", "tear"),
        ("page switch p50 (ms)", "switch"),
        ("internal SRAM free (B)", "int_free"),
        ("PSRAM free (B)", "psram_free"),
        ("radio", "radio"),
    ]

    print("| | " + " | ".join(c["label"] for c in cols) + " |")
    print("|---|" + "---:|" * len(cols))
    for name, key in rows:
        print(f"| {name} | " + " | ".join(c[key] for c in cols) + " |")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
