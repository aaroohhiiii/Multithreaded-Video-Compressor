#!/usr/bin/env python3
"""Generate the benchmark figures used by DAY18_REPORT.md.

Only the Python standard library is required. Input paths are resolved relative
to the repository root, so the script can be run from any directory.
"""

from __future__ import annotations

import csv
import html
from pathlib import Path
from typing import Iterable


ROOT = Path(__file__).resolve().parent.parent
BENCHMARK = ROOT / "benchmark"
OUTPUT = ROOT / "report" / "figures"

WIDTH = 900
HEIGHT = 520
LEFT = 88
RIGHT = 36
TOP = 72
BOTTOM = 76
PLOT_WIDTH = WIDTH - LEFT - RIGHT
PLOT_HEIGHT = HEIGHT - TOP - BOTTOM

COLORS = ["#2563eb", "#dc2626", "#059669", "#7c3aed"]


def esc(value: object) -> str:
    return html.escape(str(value), quote=True)


def read_csv(name: str) -> list[dict[str, str]]:
    with (BENCHMARK / name).open(newline="", encoding="utf-8") as source:
        return list(csv.DictReader(source))


def svg_start(title: str, description: str) -> list[str]:
    return [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" '
        f'height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}" role="img" '
        f'aria-labelledby="title desc">',
        f"<title id=\"title\">{esc(title)}</title>",
        f"<desc id=\"desc\">{esc(description)}</desc>",
        "<style>",
        "text{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;fill:#172033}",
        ".title{font-size:24px;font-weight:700}.subtitle{font-size:13px;fill:#536078}",
        ".axis{font-size:12px}.axis-title{font-size:14px;font-weight:600}",
        ".value{font-size:12px;font-weight:600}.legend{font-size:13px}",
        ".grid{stroke:#dbe1ea;stroke-width:1}.frame{fill:#fff;stroke:#aeb8c7;stroke-width:1}",
        "</style>",
        '<rect width="900" height="520" fill="#ffffff"/>',
        f'<text class="title" x="{LEFT}" y="34">{esc(title)}</text>',
        f'<text class="subtitle" x="{LEFT}" y="55">{esc(description)}</text>',
        f'<rect class="frame" x="{LEFT}" y="{TOP}" width="{PLOT_WIDTH}" height="{PLOT_HEIGHT}"/>',
    ]


def y_axis(parts: list[str], maximum: float, label: str, ticks: int = 5) -> None:
    for index in range(ticks + 1):
        value = maximum * index / ticks
        y = TOP + PLOT_HEIGHT - PLOT_HEIGHT * index / ticks
        parts.append(
            f'<line class="grid" x1="{LEFT}" y1="{y:.2f}" '
            f'x2="{LEFT + PLOT_WIDTH}" y2="{y:.2f}"/>'
        )
        parts.append(
            f'<text class="axis" x="{LEFT - 10}" y="{y + 4:.2f}" '
            f'text-anchor="end">{value:.1f}</text>'
        )
    center = TOP + PLOT_HEIGHT / 2
    parts.append(
        f'<text class="axis-title" x="22" y="{center:.2f}" '
        f'text-anchor="middle" transform="rotate(-90 22 {center:.2f})">{esc(label)}</text>'
    )


def line_chart(
    title: str,
    description: str,
    x_values: list[int],
    series: list[tuple[str, list[float]]],
    y_label: str,
    output_name: str,
    y_maximum: float,
    annotate_values: bool = True,
) -> None:
    parts = svg_start(title, description)
    y_axis(parts, y_maximum, y_label)

    def x_position(index: int) -> float:
        return LEFT + 42 + index * (PLOT_WIDTH - 84) / (len(x_values) - 1)

    def y_position(value: float) -> float:
        return TOP + PLOT_HEIGHT - value / y_maximum * PLOT_HEIGHT

    for index, value in enumerate(x_values):
        x = x_position(index)
        parts.append(
            f'<line class="grid" x1="{x:.2f}" y1="{TOP}" '
            f'x2="{x:.2f}" y2="{TOP + PLOT_HEIGHT}"/>'
        )
        parts.append(
            f'<text class="axis" x="{x:.2f}" y="{TOP + PLOT_HEIGHT + 24}" '
            f'text-anchor="middle">{value}</text>'
        )
    parts.append(
        f'<text class="axis-title" x="{LEFT + PLOT_WIDTH / 2:.2f}" '
        f'y="{HEIGHT - 20}" text-anchor="middle">Worker threads</text>'
    )

    legend_x = LEFT + PLOT_WIDTH - 145 * len(series)
    for series_index, (name, values) in enumerate(series):
        color = COLORS[series_index]
        coordinates = " ".join(
            f"{x_position(index):.2f},{y_position(value):.2f}"
            for index, value in enumerate(values)
        )
        dash = ' stroke-dasharray="7 5"' if name == "Ideal" else ""
        parts.append(
            f'<polyline points="{coordinates}" fill="none" stroke="{color}" '
            f'stroke-width="3"{dash}/>'
        )
        for index, value in enumerate(values):
            x = x_position(index)
            y = y_position(value)
            parts.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="5" fill="{color}"/>')
            if annotate_values and name != "Ideal":
                parts.append(
                    f'<text class="value" x="{x:.2f}" y="{y - 11:.2f}" '
                    f'text-anchor="middle">{value:.2f}</text>'
                )
        lx = legend_x + series_index * 145
        parts.append(
            f'<line x1="{lx}" y1="51" x2="{lx + 26}" y2="51" '
            f'stroke="{color}" stroke-width="3"{dash}/>'
        )
        parts.append(f'<text class="legend" x="{lx + 34}" y="55">{esc(name)}</text>')

    parts.append("</svg>")
    (OUTPUT / output_name).write_text("\n".join(parts) + "\n", encoding="utf-8")


def bar_chart(
    title: str,
    description: str,
    labels: list[str],
    values: list[float],
    detail_labels: Iterable[str],
    y_label: str,
    output_name: str,
) -> None:
    maximum = max(values) * 1.18
    parts = svg_start(title, description)
    y_axis(parts, maximum, y_label)
    slot = PLOT_WIDTH / len(values)
    bar_width = min(210, slot * 0.48)
    details = list(detail_labels)

    for index, (label, value) in enumerate(zip(labels, values)):
        center = LEFT + slot * (index + 0.5)
        height = value / maximum * PLOT_HEIGHT
        x = center - bar_width / 2
        y = TOP + PLOT_HEIGHT - height
        color = COLORS[index]
        parts.append(
            f'<rect x="{x:.2f}" y="{y:.2f}" width="{bar_width:.2f}" '
            f'height="{height:.2f}" fill="{color}"/>'
        )
        parts.append(
            f'<text class="value" x="{center:.2f}" y="{y - 12:.2f}" '
            f'text-anchor="middle">{value:.2f}:1</text>'
        )
        parts.append(
            f'<text class="axis-title" x="{center:.2f}" '
            f'y="{TOP + PLOT_HEIGHT + 25}" text-anchor="middle">{esc(label)}</text>'
        )
        parts.append(
            f'<text class="axis" x="{center:.2f}" '
            f'y="{TOP + PLOT_HEIGHT + 45}" text-anchor="middle">{esc(details[index])}</text>'
        )

    parts.append("</svg>")
    (OUTPUT / output_name).write_text("\n".join(parts) + "\n", encoding="utf-8")


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)

    speed_rows = [
        row for row in read_csv("results_1280x720.csv")
        if row["measurement"] == "average"
    ]
    threads = [int(row["threads"]) for row in speed_rows]
    speedups = [float(row["speedup_vs_1_thread"]) for row in speed_rows]
    line_chart(
        "Encoder speedup by thread count",
        "Three 1280x720 YUV420 frames; five timed runs per point",
        threads,
        [("Measured", speedups), ("Ideal", [float(value) for value in threads])],
        "Speedup relative to one thread (×)",
        "speedup-vs-threads.svg",
        8.6,
    )

    prediction_rows = read_csv("results_prediction_comparison.csv")
    bar_chart(
        "Compression benefit of causal prediction",
        "Same 4,147,200-byte input and codec settings; higher is better",
        ["Prediction enabled", "Prediction disabled"],
        [float(row["compression_ratio"]) for row in prediction_rows],
        [f'{int(row["output_bytes"]):,} output bytes' for row in prediction_rows],
        "Compression ratio (input bytes / output bytes)",
        "prediction-compression-ratio.svg",
    )

    granularity_rows = read_csv("results_granularity_comparison.csv")
    granularity_series: list[tuple[str, list[float]]] = []
    for granularity in (1, 2, 4):
        rows = [
            row for row in granularity_rows
            if int(row["sync_granularity"]) == granularity
        ]
        granularity_series.append(
            (f"Granularity {granularity}", [float(row["average_seconds"]) for row in rows])
        )
    line_chart(
        "Synchronization granularity comparison",
        "Average encoder wall time; lower is better",
        [1, 2, 4, 8],
        granularity_series,
        "Average wall time (seconds)",
        "sync-granularity-comparison.svg",
        0.55,
        False,
    )

    print(f"Generated 3 SVG figures in {OUTPUT}")


if __name__ == "__main__":
    main()
