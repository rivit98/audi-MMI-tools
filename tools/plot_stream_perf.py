# /// script
# requires-python = ">=3.12"
# dependencies = ["matplotlib==3.9.2"]
# ///
"""plot_stream_perf.py - audi_stream_receiver's car-run logs as plots.

    make plot RUN_DIR="<dir with the card's var/ files>"
    uv run --script tools/plot_stream_perf.py <run dir> [--out <dir>]

Reads every audi_stream_receiver*.log in the run directory (the current run
and the .prev one; older runs' usb_tx_test*.log too) with its perf CSV
(audi_stream_perf.csv / .prev.csv), and writes one PNG per log next to it
(<log name>.png, or into --out). Panels share one time axis, seconds since
the app started (the log's [N ms]):

  CPU            cpu: lines: self, unit, others, the main thread; the main
                 loop's busy % from the JPEG perf windows
  H.264 rates    h264: windows, per second: frames, skips, io-media reads,
                 reads that waited, evicted reads (newer logs only)
  H.264 lag      the window's lag median, min-max band
  JPEG rates     perf: windows, per second: shown, dropped, held back by
                 the cap (delayed + acks held; newer logs only)
  JPEG picture   the CSV's shown pictures: decode / render / copy / flip ms
                 stacked, and latency

Events are vertical lines across all panels: stream start / end, config,
io-media error, REFUSED, rear camera up / off, USB link up / down, HIGH
load and WARNING lines. A short summary per log goes to stdout.

Logs without the newer fields (evicted, cap) still plot; those series are
just missing.
"""
from __future__ import annotations

import argparse
import csv
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

LINE = re.compile(r"^\[\s*(\d+) ms\] (.*)$")

CPU = re.compile(
    r"^cpu: ([\d.]+) s: self (\d+)% \(max \d+%\), unit (\d+)% \(max \d+%\), others (\d+)%"
    r"(?: \| (.*?))?( - HIGH)?$")
H264 = re.compile(
    r"^h264: ([\d.]+) s: (\d+) frames \([\d.]+ fps\), (\d+) skips, (\d+) refused; "
    r"io-media (\d+) reads, (\d+) waited \(max \d+ ms\), lead [^;]*; "
    r"lag (-?\d+)/(-?\d+)/(-?\d+) ms \((\d+) TIME\)")
H264_NEW = re.compile(r"; evicted (\d+), far (\d+), eof (\d+), unblocked (\d+); reader (-?\d+) frames behind")
PERF = re.compile(
    r"^perf: window ([\d.]+) s: (\d+) shown [\d.]+ fps, (\d+) dropped, (\d+) bad .*"
    r"latency (\d+)/(\d+) .*main busy (\d+)%")
PERF_CAP = re.compile(r"cap \d+ fps: (\d+) delayed \S+, (\d+) acks held")

# (kind, pattern): the first that matches a line names its event
EVENTS = [
    ("io-media error", re.compile(r"^h264: io-media error")),
    ("REFUSED", re.compile(r"H264_STATUS REFUSED")),
    ("stream start", re.compile(r"^h264: streaming at |^video: JPEG fallback|^video: #1 id")),
    ("stream end", re.compile(r"^h264: stream ended|^video: stopped")),
    ("config", re.compile(r"^h264: config:")),
    ("camera up", re.compile(r"^camera: up")),
    ("camera off", re.compile(r"^camera: off")),
    ("USB link up", re.compile(r"^state \S+ -> CONNECTED")),
    ("USB link down", re.compile(r"^state CONNECTED -> |^usb: remove ")),
    ("HIGH load", re.compile(r"^perf: HIGH main load")),
    ("warning", re.compile(r"WARNING|^perf: JPEG cap hit")),
]

# fixed categorical order (series 1..5), and event colours: status colours
# for what went wrong, quiet ones for the rest
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6"]
EVENT_STYLE = {
    "io-media error": ("#d03b3b", "-", 1.6),
    "REFUSED": ("#d03b3b", "--", 1.2),
    "stream start": ("#52514e", "-", 0.8),
    "stream end": ("#52514e", ":", 0.8),
    "config": ("#9a9994", ":", 0.6),
    "camera up": ("#6250d6", "-", 0.9),
    "camera off": ("#6250d6", ":", 0.9),
    "USB link up": ("#008300", "-", 0.9),
    "USB link down": ("#008300", ":", 0.9),
    "HIGH load": ("#ec835a", "-", 0.9),
    "warning": ("#fab219", "-", 0.9),
}


@dataclass
class Run:
    log: Path
    csv: Path | None = None
    end_s: float = 0.0
    cpu: list[dict] = field(default_factory=list)
    h264: list[dict] = field(default_factory=list)
    jpeg: list[dict] = field(default_factory=list)
    pics: list[dict] = field(default_factory=list)
    events: list[tuple[float, str, str]] = field(default_factory=list)


def threads(text: str | None) -> dict[str, int]:
    """'main 18%, usbd 6%, tid 7 1%' -> {'main': 18, 'usbd': 6, 'tid 7': 1}"""
    out = {}
    for part in (text or "").split(","):
        m = re.match(r"^\s*(.+?) (\d+)%\s*$", part)
        if m:
            out[m.group(1)] = int(m.group(2))
    return out


def parse_log(path: Path) -> Run:
    run = Run(path)
    for raw in path.read_text(errors="replace").splitlines():
        m = LINE.match(raw)
        if not m:
            continue
        t = int(m.group(1)) / 1000.0
        msg = m.group(2)
        run.end_s = max(run.end_s, t)
        if c := CPU.match(msg):
            run.cpu.append({"t": t, "span": float(c.group(1)), "self": int(c.group(2)),
                            "unit": int(c.group(3)), "others": int(c.group(4)),
                            "main": threads(c.group(5)).get("main", 0), "high": bool(c.group(6))})
            continue
        if h := H264.match(msg):
            span = float(h.group(1)) or 1.0
            row = {"t": t, "span": span, "frames": int(h.group(2)), "skips": int(h.group(3)),
                   "refused": int(h.group(4)), "reads": int(h.group(5)), "waited": int(h.group(6)),
                   "lag_min": int(h.group(7)), "lag_p50": int(h.group(8)),
                   "lag_max": int(h.group(9)), "times": int(h.group(10)), "evicted": None}
            if n := H264_NEW.search(msg):
                row.update(evicted=int(n.group(1)), far=int(n.group(2)), eof=int(n.group(3)),
                           unblocked=int(n.group(4)), behind=int(n.group(5)))
            run.h264.append(row)
            continue
        if p := PERF.match(msg):
            row = {"t": t, "span": float(p.group(1)) or 1.0, "shown": int(p.group(2)),
                   "dropped": int(p.group(3)), "bad": int(p.group(4)),
                   "latency": int(p.group(5)), "busy": int(p.group(7)), "delayed": None,
                   "held": None}
            if cp := PERF_CAP.search(msg):
                row.update(delayed=int(cp.group(1)), held=int(cp.group(2)))
            run.jpeg.append(row)
            continue
        for kind, rx in EVENTS:
            if rx.search(msg):
                run.events.append((t, kind, msg))
                break
    return run


def parse_csv(path: Path) -> list[dict]:
    """Shown pictures: t (s) and the stages in ms; older files lack the cap columns."""
    pics = []
    with path.open(newline="") as f:
        for r in csv.DictReader(f):
            try:
                if int(r["status"]) != 0:
                    continue
                ms = lambda k: int(r.get(k) or 0) / 1000.0
                pics.append({"t": int(r["t_ms"]) / 1000.0, "decode": ms("decode_us"),
                             "render": ms("render_us"), "copy": ms("copy_us"),
                             "flip": ms("flip_us"), "latency": ms("latency_us"),
                             "cap": ms("cap_us"), "hold": ms("hold_us")})
            except (KeyError, ValueError, TypeError):
                continue                 # a line cut short by a pulled card
    return pics


def csv_for(log: Path) -> Path | None:
    """audi_stream_receiver.prev.log -> audi_stream_perf.prev.csv (usb_tx_test -> usb_tx_perf)."""
    name = log.name[: -len(".log")]
    for app, perf in (("audi_stream_receiver", "audi_stream_perf"), ("usb_tx_test", "usb_tx_perf")):
        if name.startswith(app):
            p = log.with_name(perf + name[len(app):] + ".csv")
            return p if p.exists() else None
    return None


def find_logs(run_dir: Path) -> list[Path]:
    logs = []
    for pat in ("audi_stream_receiver*.log", "usb_tx_test*.log"):
        logs += [p for p in sorted(run_dir.glob(pat)) if not p.name.endswith("_out.log")]
    return logs


def summary(run: Run) -> list[str]:
    out = [f"{run.log.name}: {run.end_s:.0f} s of log"]
    h_s = sum(r["span"] for r in run.h264)
    if h_s:
        fps = [r["frames"] / r["span"] for r in run.h264]
        lag = [r["lag_p50"] for r in run.h264 if r["times"]]
        ev = (sum(r["evicted"] or 0 for r in run.h264)
              if any(r["evicted"] is not None for r in run.h264) else "n/a (older log)")
        out.append(f"  H.264: {h_s:.0f} s in {len(run.h264)} windows, frames avg "
                   f"{sum(r['frames'] for r in run.h264) / h_s:.1f} / max {max(fps):.1f} fps, "
                   f"lag median max {max(lag) if lag else 0} ms, evicted {ev}")
    j_s = sum(r["span"] for r in run.jpeg)
    if j_s:
        fps = [r["shown"] / r["span"] for r in run.jpeg]
        hot = sum(r["span"] for r in run.jpeg if r["busy"] >= 80)
        capped = (sum((r["delayed"] or 0) + (r["held"] or 0) for r in run.jpeg)
                  if any(r["delayed"] is not None for r in run.jpeg) else "n/a (older log)")
        out.append(f"  JPEG: {j_s:.0f} s in {len(run.jpeg)} windows, shown avg "
                   f"{sum(r['shown'] for r in run.jpeg) / j_s:.1f} / max {max(fps):.1f} fps, "
                   f"dropped {sum(r['dropped'] for r in run.jpeg)}, main loop >= 80 % busy "
                   f"{100 * hot / j_s:.0f} % of the time, held back by the cap {capped}")
    c_s = sum(r["span"] for r in run.cpu)
    if c_s:
        hot = sum(r["span"] for r in run.cpu if r["main"] >= 80)
        out.append(f"  CPU: self avg {sum(r['self'] * r['span'] for r in run.cpu) / c_s:.0f} %, "
                   f"unit avg {sum(r['unit'] * r['span'] for r in run.cpu) / c_s:.0f} %, main "
                   f"thread >= 80 % in {100 * hot / c_s:.0f} % of the time, "
                   f"{sum(r['high'] for r in run.cpu)} HIGH windows")
    count = lambda k: sum(1 for e in run.events if e[1] == k)
    out.append(f"  events: {count('io-media error')} io-media errors, {count('REFUSED')} REFUSED, "
               f"{count('warning')} warnings, {count('HIGH load')} HIGH main load, "
               f"{count('camera up')} camera, {count('USB link down')} USB link down")
    return out


def segments(rows: list[dict], gap_s: float = 3.0) -> list[list[dict]]:
    """Split per-picture rows where the stream paused, so areas don't bridge gaps."""
    segs: list[list[dict]] = []
    for r in rows:
        if not segs or r["t"] - segs[-1][-1]["t"] > gap_s:
            segs.append([])
        segs[-1].append(r)
    return segs


def window_x(rows: list[dict]) -> list[float]:
    """A window line comes at its end; plot it at its middle."""
    return [r["t"] - r["span"] / 2 for r in rows]


def plot(run: Run, out: Path, title: str) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.lines import Line2D

    panels = ["cpu"]
    if run.h264:
        panels += ["h264", "lag"]
    if run.jpeg or run.pics:
        panels += ["jpeg", "pics"]
    fig, axes = plt.subplots(len(panels), 1, sharex=True, figsize=(16, 2.6 * len(panels) + 1.2),
                             squeeze=False)
    axes = [a[0] for a in axes]
    ink, muted, grid = "#0b0b0b", "#52514e", "#e4e3df"
    fig.patch.set_facecolor("#fcfcfb")

    def line(ax, x, y, label, i, **kw):
        ax.plot(x, y, color=SERIES[i], lw=1.6, label=label, **kw)

    for ax, name in zip(axes, panels):
        ax.set_facecolor("#fcfcfb")
        ax.grid(True, color=grid, lw=0.6)
        ax.tick_params(colors=muted, labelsize=8)
        for s in ax.spines.values():
            s.set_color(grid)
        if name == "cpu":
            c = run.cpu
            x = window_x(c)
            line(ax, x, [r["self"] for r in c], "self", 0)
            line(ax, x, [r["unit"] for r in c], "unit", 1)
            line(ax, x, [r["others"] for r in c], "others", 2)
            line(ax, x, [r["main"] for r in c], "main thread", 3)
            if run.jpeg:
                ax.plot(window_x(run.jpeg), [r["busy"] for r in run.jpeg], "o", ms=4,
                        color=SERIES[4], label="main loop busy (JPEG)")
            for r in c:
                if r["high"]:
                    ax.axvspan(r["t"] - r["span"], r["t"], color="#ec835a", alpha=0.08, lw=0)
            ax.set_ylim(0, 105)
            ax.set_ylabel("CPU %", color=ink, fontsize=9)
            ax.set_title("CPU (shaded: cpu window with own load HIGH)", loc="left", fontsize=9,
                         color=muted)
        elif name == "h264":
            h = run.h264
            x = window_x(h)
            per = lambda k: [(r[k] or 0) / r["span"] for r in h]
            line(ax, x, per("frames"), "frames/s", 0)
            line(ax, x, per("skips"), "skips/s", 1)
            line(ax, x, per("reads"), "io-media reads/s", 2)
            line(ax, x, per("waited"), "reads waited/s", 3)
            if any(r["evicted"] is not None for r in h):
                line(ax, x, [(r["evicted"] or 0) / r["span"] if r["evicted"] is not None
                             else float("nan") for r in h], "evicted/s", 4, marker="o", ms=3)
            ax.set_ylabel("per second", color=ink, fontsize=9)
            ax.set_title("H.264 (10 s windows)", loc="left", fontsize=9, color=muted)
        elif name == "lag":
            h = [r for r in run.h264 if r["times"]]
            x = window_x(h)
            ax.fill_between(x, [r["lag_min"] for r in h], [r["lag_max"] for r in h],
                            color=SERIES[0], alpha=0.18, lw=0, label="lag min-max")
            line(ax, x, [r["lag_p50"] for r in h], "lag median", 0)
            ax.set_ylim(bottom=0)
            ax.set_ylabel("ms", color=ink, fontsize=9)
            ax.set_title("H.264 lag (MMI side)", loc="left", fontsize=9, color=muted)
        elif name == "jpeg":
            j = run.jpeg
            x = window_x(j)
            per = lambda k: [(r[k] or 0) / r["span"] for r in j]
            line(ax, x, per("shown"), "shown/s", 0, marker="o", ms=3)
            line(ax, x, per("dropped"), "dropped/s", 1, marker="o", ms=3)
            if any(r["delayed"] is not None for r in j):
                line(ax, x, [((r["delayed"] or 0) + (r["held"] or 0)) / r["span"] for r in j],
                     "held back by the cap/s", 2, marker="o", ms=3)
            ax.set_ylim(bottom=0)
            ax.set_ylabel("per second", color=ink, fontsize=9)
            ax.set_title("JPEG (perf windows)", loc="left", fontsize=9, color=muted)
        elif name == "pics":
            keys = [("decode", 0), ("render", 1), ("copy", 2), ("flip", 3)]
            for n, seg in enumerate(segments(run.pics)):
                xs = [r["t"] for r in seg]
                ax.stackplot(xs, *[[r[k] for r in seg] for k, _ in keys],
                             colors=[SERIES[i] for _, i in keys], alpha=0.85, lw=0,
                             labels=[k for k, _ in keys] if n == 0 else ())
                ax.plot(xs, [r["latency"] for r in seg], color=ink, lw=0.8,
                        label="latency" if n == 0 else None)
            # a few stalls of seconds would flatten the rest: clip at the
            # 98th percentile, and say so
            tops = sorted(max(r["latency"], r["decode"] + r["render"] + r["copy"] + r["flip"])
                          for r in run.pics)
            top = tops[int(len(tops) * 0.98)] * 1.3 if tops else 0.0
            clipped = bool(tops) and tops[-1] > top > 0
            if clipped:
                ax.set_ylim(0, top)
            else:
                ax.set_ylim(bottom=0)
            ax.set_ylabel("ms per picture", color=ink, fontsize=9)
            ax.set_title("JPEG pictures (CSV): main-thread stages stacked, latency line" +
                         (f" (clipped at {top:.0f} ms, max {tops[-1]:.0f} ms)" if clipped else ""),
                         loc="left", fontsize=9, color=muted)
        ax.legend(loc="upper left", bbox_to_anchor=(1.005, 1.0), fontsize=8, frameon=False)

    seen = []
    order = list(EVENT_STYLE)
    # the errors last, over the stream end / start that come with them
    for t, kind, _ in sorted(run.events, key=lambda e: -order.index(e[1])):
        color, ls, lw = EVENT_STYLE[kind]
        for ax in axes:
            ax.axvline(t, color=color, ls=ls, lw=lw, alpha=0.9, zorder=0.5)
        if kind not in seen:
            seen.append(kind)
    if seen:
        handles = [Line2D([], [], color=EVENT_STYLE[k][0], ls=EVENT_STYLE[k][1], lw=1.4, label=k)
                   for k in EVENT_STYLE if k in seen]
        fig.legend(handles=handles, loc="lower center", ncol=min(len(handles), 6), fontsize=8,
                   frameon=False, bbox_to_anchor=(0.45, 0.0))
    axes[-1].set_xlabel("seconds since the app started", color=ink, fontsize=9)
    axes[-1].set_xlim(0, max(run.end_s, 1))
    fig.suptitle(f"{title} / {run.log.name}", x=0.01, ha="left", fontsize=11,
                 color=ink)
    fig.tight_layout(rect=(0, 0.04 if seen else 0, 1, 0.98))
    fig.savefig(out, dpi=110, facecolor=fig.get_facecolor())
    plt.close(fig)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("run_dir", type=Path)
    ap.add_argument("--out", type=Path, help="where the PNGs go (default: the run directory)")
    ap.add_argument("--title", help="the run's name in the plots (default: the directory's)")
    ap.add_argument("--check", action="store_true", help="import matplotlib and exit")
    a = ap.parse_args(argv)
    if a.check:
        import matplotlib  # noqa: F401
        return 0
    logs = find_logs(a.run_dir)
    if not logs:
        print(f"{a.run_dir}: no audi_stream_receiver*.log", file=sys.stderr)
        return 1
    out_dir = a.out or a.run_dir
    for log in logs:
        run = parse_log(log)
        run.csv = csv_for(log)
        if run.csv:
            run.pics = parse_csv(run.csv)
        png = out_dir / (log.name[: -len(".log")] + ".png")
        plot(run, png, a.title or a.run_dir.resolve().name)
        print("\n".join(summary(run)))
        print(f"  -> {png}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
