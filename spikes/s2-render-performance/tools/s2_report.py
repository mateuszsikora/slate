#!/usr/bin/env python3
"""
Turn an S-2 serial log into the numbers docs/spikes/s2.md reports.

    idf.py -p /dev/cu.usbmodem* monitor | tee run.log
    python3 tools/s2_report.py run.log

Percentiles rather than averages, throughout. Stutter is a tail phenomenon: a
page switch that hitches once every twenty frames is exactly what the §13
criterion is about, and a mean frame time hides it behind nineteen good ones.

Steady-state frames and page-switch frames are reported separately. Mixing them
would let a 100 ms full-screen repaint contaminate the idle number, or the other
way round — and they are answers to two different questions.

Exit code is 1 when a hard limit is breached, so this can gate a re-run.
"""

import sys
from collections import defaultdict


def pct(values, p):
    if not values:
        return 0
    s = sorted(values)
    i = min(len(s) - 1, int(round((p / 100.0) * (len(s) - 1))))
    return s[i]


def parse(path):
    cfg, frames, switches = {}, [], []
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if line.startswith("S2CFG,"):
            f = line.split(",")[1:]
            cfg = {f[i]: f[i + 1] for i in range(0, len(f) - 1, 2)}
        elif line.startswith("S2F,"):
            f = line.split(",")
            if len(f) < 8:
                continue
            try:
                frames.append({
                    "idx": int(f[1]), "start": int(f[2]), "render": int(f[3]),
                    "flush": int(f[4]), "flush_cnt": int(f[5]),
                    "tear": int(f[6]), "px": int(f[7]),
                    "wait": int(f[8]) if len(f) > 8 else 0,
                })
            except ValueError:
                continue  # a line the UART chopped in half
        elif line.startswith("S2SW,"):
            f = line.split(",")
            if len(f) < 7:
                continue
            try:
                switches.append({
                    "round": int(f[1]), "destroy": int(f[2]), "build": int(f[3]),
                    "first_frame": int(f[4]), "frame_index": int(f[5]),
                    "tiles": int(f[6]),
                })
            except ValueError:
                continue
    return cfg, frames, switches


def table(title, rows):
    print(f"\n{title}")
    print("-" * len(title))
    w = max(len(r[0]) for r in rows)
    for name, value in rows:
        print(f"  {name:<{w}}  {value}")


def main(path):
    cfg, frames, switches = parse(path)
    if not frames:
        sys.exit(f"{path}: no S2F rows — did the run reach S2END?")

    vsync_us = int(cfg.get("vsync_period_us", 0))
    # Frames produced by a page switch: the first render after each rebuild is a
    # full-screen repaint and belongs in the switch column, not the idle one.
    switch_frames = {s["frame_index"] for s in switches}

    steady = [f for f in frames if f["idx"] not in switch_frames]
    switch = [f for f in frames if f["idx"] in switch_frames]

    span_us = frames[-1]["start"] - frames[0]["start"]
    intervals = [b["start"] - a["start"] for a, b in zip(frames, frames[1:])]

    print(f"S-2 render performance — {path}")
    table("Configuration", [
        ("framebuffers", cfg.get("num_fbs", "?")),
        ("bounce buffer (px)", cfg.get("bounce_px", "?")),
        ("tiles", f'{cfg.get("tiles_built", "?")} on a {cfg.get("grid", "?")} grid'),
        ("panel refresh, calculated", f'{cfg.get("refresh_hz_calc", "?")} Hz'),
        ("panel refresh, measured", f"{1e6 / vsync_us:.2f} Hz" if vsync_us else "n/a"),
        ("frame budget (1 refresh)", f"{vsync_us} us" if vsync_us else "n/a"),
        ("LVGL heap peak", f'{int(cfg.get("lvgl_max_used", 0)):,} B'),
        ("internal heap free", f'{int(cfg.get("int_free", 0)):,} B'),
        ("PSRAM free", f'{int(cfg.get("psram_free", 0)):,} B'),
    ])

    # Cost of producing one frame.
    #
    # LVGL's render window (RENDER_START..RENDER_READY) already contains the
    # flush calls, so adding flush to it double-counts. And when VSYNC gating is
    # on, the flush blocks until the panel has taken the buffer, so the window
    # also contains idle time — 15.4 ms of a 24.7 ms window in the gated runs.
    # Reporting that as the cost of drawing would make the configuration that
    # waits for the panel look three times more expensive than the one that
    # does not, when in fact it draws in about the same time and then waits.
    work = [f["render"] - f["wait"] for f in steady]
    table(f"Steady state — {len(steady)} frames over {span_us / 1e6:.1f} s", [
        ("effective frame rate", f"{len(frames) * 1e6 / span_us:.1f} fps"),
        ("render p50 / p95 / p99 / max",
         " / ".join(f"{pct([f['render'] for f in steady], p):,}" for p in (50, 95, 99, 100)) + " us"),
        ("flush  p50 / p95 / p99 / max",
         " / ".join(f"{pct([f['flush'] for f in steady], p):,}" for p in (50, 95, 99, 100)) + " us"),
        ("vsync wait p50 / max",
         f"{pct([f['wait'] for f in steady], 50):,} / {max(f['wait'] for f in steady):,} us"),
        ("frame work p50 / p95 / p99 / max",
         " / ".join(f"{pct(work, p):,}" for p in (50, 95, 99, 100)) + " us"),
        ("interval p50 / p95 / max",
         " / ".join(f"{pct(intervals, p):,}" for p in (50, 95, 100)) + " us"),
        # Frame intervals, not just frame times. A frame that was never rendered
        # cannot appear in a frame-time percentile, so a screen that stops being
        # drawn for 300 ms looks flawless by every other number here. This is the
        # line that exposed the harness animating one tile instead of four.
        ("gaps > 100 ms",
         f"{len([d for d in intervals if d > 100000])}"
         f" ({len([d for d in intervals if d > 100000]) / (span_us / 1e6):.2f}/s,"
         f" {100 * sum(d for d in intervals if d > 100000) / span_us:.0f} % of wall time)"),
        ("frames over one refresh period",
         f"{sum(1 for w in work if vsync_us and w > vsync_us)} / {len(steady)}"
         f" ({100.0 * sum(1 for w in work if vsync_us and w > vsync_us) / max(1, len(steady)):.1f} %)"),
        ("pixels per frame p50 / max",
         " / ".join(f"{pct([f['px'] for f in steady], p):,}" for p in (50, 100))),
    ])

    flushes = sum(f["flush_cnt"] for f in frames)
    tears = sum(f["tear"] for f in frames)
    torn_frames = sum(1 for f in frames if f["tear"])
    table("Tearing", [
        ("flushes total", f"{flushes:,}"),
        ("flushes that raced the scan-out", f"{tears:,} ({100.0 * tears / max(1, flushes):.1f} %)"),
        ("frames containing at least one", f"{torn_frames:,} / {len(frames):,}"
                                           f" ({100.0 * torn_frames / max(1, len(frames)):.1f} %)"),
        ("torn frames per second", f"{torn_frames * 1e6 / span_us:.1f}"),
    ])

    if switches:
        totals = [s["destroy"] + s["build"] + s["first_frame"] for s in switches]
        table(f"Page switch — {len(switches)} rounds", [
            ("destroy p50 / max",
             " / ".join(f"{pct([s['destroy'] for s in switches], p):,}" for p in (50, 100)) + " us"),
            ("build p50 / max",
             " / ".join(f"{pct([s['build'] for s in switches], p):,}" for p in (50, 100)) + " us"),
            ("first frame on panel p50 / max",
             " / ".join(f"{pct([s['first_frame'] for s in switches], p):,}" for p in (50, 100)) + " us"),
            ("total p50 / max",
             " / ".join(f"{pct(totals, p) / 1000.0:.1f}" for p in (50, 100)) + " ms"),
            ("worst switch frame render+flush",
             f"{max((f['render'] + f['flush'] for f in switch), default=0):,} us"),
        ])

    # A verdict the script can state on its own. The visual half of §13 is not
    # something a log can answer, so this prints what was measured and says so.
    print()
    over = sum(1 for w in work if vsync_us and w > vsync_us)
    verdict = []
    if vsync_us and pct(work, 95) > vsync_us:
        verdict.append(f"p95 steady-state frame work {pct(work, 95)} us exceeds the "
                       f"{vsync_us} us refresh period")
    if over > 0.05 * max(1, len(steady)):
        verdict.append(f"{100.0 * over / len(steady):.1f} % of frames miss the refresh")
    print("MEASURED: " + ("; ".join(verdict) if verdict
                          else "steady state fits inside the panel's refresh period"))
    print("The §13 criterion is visual. This is the quantitative half only.")
    return 1 if verdict else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "run.log"))
