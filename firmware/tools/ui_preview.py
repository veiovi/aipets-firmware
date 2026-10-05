#!/usr/bin/env python3
"""Render the device menu pages with real LVGL on this computer.

Builds firmware/tests/onboarding_host/ui_preview.c as the Pocket Terminal
against the pinned LVGL and writes one native-resolution PNG per page plus a
contact sheet, so a design can be reviewed before it is flashed.

    python3 firmware/tools/ui_preview.py --output <directory>
    python3 firmware/tools/ui_preview.py --board amoled-206 --output <directory>
    python3 firmware/tools/ui_preview.py --board lcd-154 --output <directory>

Needs a C compiler (CC) and LVGL_SOURCE_DIR (or firmware/managed_components)
and Pillow. LVGL objects are cached between runs.
"""
import argparse
import hashlib
import os
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import tempfile

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
LVGL = Path(os.environ.get("LVGL_SOURCE_DIR", ROOT / "managed_components/lvgl__lvgl"))
CC = os.environ.get("CC", "cc")
# LVGL as the Pocket build configures it (16-bit colour, Montserrat 14 default).
LVGL_FLAGS = ["-std=c11", "-O1", "-DLV_CONF_SKIP", "-DLV_USE_STDLIB_MALLOC=1", "-DLV_USE_STDLIB_STRING=1",
              "-DLV_USE_STDLIB_SPRINTF=1", "-DLV_COLOR_DEPTH=16", "-I", str(LVGL)]
PAGES = ["boot", "home", "home-cards", "home-log", "home-about", "wifi", "password", "code", "link",
         "link-question", "move-code", "move-question", "restart", "about", "about-notices"]


def lvgl_objects(cache: Path) -> list[str]:
    cache.mkdir(parents=True, exist_ok=True)

    def build(item: tuple[int, Path]) -> str:
        index, source = item
        obj = cache / f"lvgl-{index}.o"
        if not obj.exists() or obj.stat().st_mtime < source.stat().st_mtime:
            subprocess.run([CC, *LVGL_FLAGS, "-c", str(source), "-o", str(obj)], check=True, capture_output=True)
        return str(obj)

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        return list(pool.map(build, enumerate(sorted((LVGL / "src").rglob("*.c")))))


def panel_png(ppm: Path, png: Path, round_panel: bool) -> Image.Image:
    image = Image.open(ppm).convert("RGB")
    mask = Image.new("L", image.size, 0)
    bounds = (0, 0, image.width - 1, image.height - 1)
    if round_panel:
        ImageDraw.Draw(mask).ellipse(bounds, fill=255)
    else:
        ImageDraw.Draw(mask).rounded_rectangle(bounds, radius=22, fill=255)
    framed = Image.new("RGB", image.size, (22, 22, 24))
    framed.paste(image, (0, 0), mask)
    framed.save(png)
    return framed


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--board", choices=("round-185b", "amoled-206", "lcd-154"), default="round-185b")
    arguments = parser.parse_args()
    output = arguments.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256(" ".join([CC, *LVGL_FLAGS]).encode()).hexdigest()[:12]
    objects = lvgl_objects(Path(tempfile.gettempdir()) / f"aipet-ui-preview-lvgl-{digest}")
    round_panel = arguments.board == "round-185b"
    board_flags = {"round-185b": [], "amoled-206": ["-DPET_BOARD_AMOLED_206=1"],
                   "lcd-154": ["-DPET_BOARD_LCD_154=1"]}[arguments.board]
    binary = output / "ui_preview"
    subprocess.run([CC, *LVGL_FLAGS, "-g", "-Wall", "-Wextra", "-Werror", "-DCONFIG_PET_POCKET_TERMINAL=1",
                    "-I", str(ROOT / "tests/onboarding_host/include"), "-I", str(ROOT / "main"),
                    str(ROOT / "tests/onboarding_host/ui_preview.c"), str(ROOT / "main/pet_enrollment.c"),
                    str(ROOT / "main/pet_menu_font.c"), str(ROOT / "main/pet_menu_icons.c"),
                    *board_flags, *objects, "-lm", "-o", str(binary)], check=True)
    # LVGL's default assert handler spins forever: a stuck page fails here instead.
    subprocess.run([str(binary), str(output)], check=True, capture_output=True, timeout=120)
    columns = 4
    rows = (len(PAGES) + columns - 1) // columns
    cell_width, cell_height = {"round-185b": (380, 380), "amoled-206": (430, 522),
                               "lcd-154": (260, 260)}[arguments.board]
    sheet = Image.new("RGB", (columns * cell_width + 20, rows * cell_height + 20), (12, 12, 14))
    for index, page in enumerate(PAGES):
        image = panel_png(output / f"{page}.ppm", output / f"{page}.png", round_panel)
        (output / f"{page}.ppm").unlink()
        sheet.paste(image, (20 + (index % columns) * cell_width, 20 + (index // columns) * cell_height))
    sheet.save(output / "sheet.png")
    binary.unlink()
    print(output / "sheet.png")


if __name__ == "__main__":
    main()
