"""One figure style for the whole repository.

Every chart routes through here so that the figures read as one set rather than
as five independent scripts' defaults. The rules encoded below:

  * SVG. Every figure here carries text and thin lines, which a raster format
    blurs at the width GitHub renders README images (~880px).
  * A colourblind-safe qualitative palette (Okabe-Ito), defined once.
  * Direct labels on series instead of a legend wherever there are few enough
    series to place them.
  * Titles state the finding, not the variable names. The subtitle carries the
    sample, the period and the units.
  * Every figure carries a provenance footer naming the source artifact, the
    machine and the commit, because a latency chart without its machine is
    decoration.
  * No top/right spines, a light horizontal grid only, no background fill.
"""
from __future__ import annotations

import json
import pathlib

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parent.parent
DATA = REPO / "reports" / "data"
FIGURES = REPO / "reports" / "figures"

# Okabe-Ito. Distinguishable under the common forms of colour vision deficiency.
ORANGE = "#E69F00"
SKY = "#56B4E9"
GREEN = "#009E73"
YELLOW = "#F0E442"
BLUE = "#0072B2"
VERMILLION = "#D55E00"
PURPLE = "#CC79A7"
GREY = "#5A5A5A"
PALETTE = [BLUE, VERMILLION, GREEN, ORANGE, PURPLE, SKY]

# 1200x750 px at 100 dpi. Never wider than 1400px: GitHub downsamples anything
# larger and the text goes soft.
FIGSIZE = (12.0, 7.5)
DPI = 100


def apply_rc() -> None:
    plt.rcParams.update({
        "figure.figsize": FIGSIZE,
        "figure.dpi": DPI,
        "savefig.dpi": DPI,
        "figure.facecolor": "white",
        "axes.facecolor": "white",
        "savefig.facecolor": "white",
        "font.family": "sans-serif",
        "font.sans-serif": ["Helvetica Neue", "Helvetica", "Arial",
                            "DejaVu Sans", "Liberation Sans", "sans-serif"],
        "font.size": 12,
        "axes.titlesize": 15,
        "axes.titleweight": "semibold",
        "axes.labelsize": 12,
        "xtick.labelsize": 10,
        "ytick.labelsize": 10,
        "legend.fontsize": 10,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "axes.grid": True,
        "axes.axisbelow": True,
        "grid.color": "#D8D8D8",
        "grid.linewidth": 0.8,
        "grid.linestyle": "-",
        "svg.fonttype": "none",  # keep text as text, not paths
    })


def read_meta(artifact: str) -> dict:
    path = DATA / f"{artifact}.meta.json"
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text())
    except json.JSONDecodeError:
        return {}


def source_line(artifact: str, synthetic: bool = True) -> str:
    """The 8pt italic provenance footer that every figure carries."""
    meta = read_meta(artifact)
    machine = meta.get("machine", {})
    bits = [f"Source: reports/data/{artifact}"]
    if machine.get("cpu"):
        bits.append(f"{machine['cpu']}, {machine.get('logical_cores', '?')} cores")
    if machine.get("os"):
        bits.append(machine["os"])
    if meta.get("generated_utc"):
        bits.append(meta["generated_utc"])
    commit = meta.get("git_commit", "")
    if commit:
        bits.append(f"commit {commit[:12]}")
    line = " · ".join(bits)
    if synthetic:
        line += "  ·  SYNTHETIC market data (bundled SimExchange, not a live venue)"
    return line


def titled(ax, title: str, subtitle: str) -> None:
    """Finding as the title, sample/period/units underneath it."""
    ax.set_title(title, loc="left", pad=26)
    ax.text(0.0, 1.02, subtitle, transform=ax.transAxes, fontsize=11,
            color=GREY, va="bottom", ha="left")


def finish(fig, ax, artifact: str, synthetic: bool = True, **margins) -> None:
    """Draw the provenance footer and set consistent margins.

    Margins are explicit rather than left to bbox_inches so successive figures
    line up when they are stacked in a README. A figure with long tick labels
    passes its own `left`.
    """
    fig.text(0.01, 0.015, source_line(artifact, synthetic), fontsize=8,
             style="italic", color=GREY, ha="left", va="bottom")
    box = {"left": 0.09, "right": 0.97, "top": 0.86, "bottom": 0.14}
    box.update(margins)
    fig.subplots_adjust(**box)


def save(fig, name: str) -> pathlib.Path:
    FIGURES.mkdir(parents=True, exist_ok=True)
    out = FIGURES / name
    fig.savefig(out, format="svg")
    plt.close(fig)
    return out


def annotate(ax, text: str, xy, xytext, color: str = GREY) -> None:
    """A short callout with a leader line, for the reader's takeaway."""
    ax.annotate(text, xy=xy, xytext=xytext, fontsize=10, color=color,
                arrowprops={"arrowstyle": "-", "color": color, "lw": 0.9,
                            "shrinkA": 2, "shrinkB": 4})
