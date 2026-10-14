#!/usr/bin/env python3
"""Generate the device menu's bitmap faces, icons and boot mark as LVGL C sources.

    python3 firmware/tools/make_menu_assets.py [--logo <png>]

Fonts: the ASCII glyphs of Share Tech Mono (third_party/share-tech-mono),
rasterized by FreeType at the menu's sizes into LVGL's 4-bit format. That is a
Modified Version under the SIL Open Font License 1.1: main/pet_menu_font.c
keeps that license, is named pet_menu_font_* because 'Share' is a Reserved
Font Name, and carries the copyright notice and license text as
pet_menu_font_license, which the menu's ABOUT page shows, so every copy of
the firmware includes them. Icons are drawn here, supersampled, as 8-bit alpha
masks the menu tints (main/pet_menu_icons.c); the story screens' pixel art is
drawn here bit by bit. Needs Pillow with FreeType.

The WOODLAND style: the ASCII glyphs of Jersey 10, 15 and 20
(third_party/jersey) and of Jacquard 12 (third_party/jacquard-12), each at the
size where one of its pixels is one of the screen's, 1 bit per pixel, in
main/pet_menu_woodland_font.c (SIL OFL 1.1, no Reserved Font Names); and its
plank and rope tiles, drawn here pixel by pixel, as RGB565 images in
main/pet_menu_icons.c.

The boot page shows a mark above its log: pet_boot_mark in main/pet_menu_icons.c,
an 8-bit coverage mask the page tints lime. --logo makes it from a PNG's alpha;
without it the mark is a plain ring.
"""
import argparse
import hashlib
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONT = ROOT / "third_party/share-tech-mono/ShareTechMono-Regular.ttf"
SIZES = (12, 16, 20, 28)
FIRST, LAST = 0x20, 0x7E
SUPERSAMPLE = 4
LOGO_WIDTH = 200  # px on the 360 px glass: about the width of the boot log below it
LOGO_NOISE = 16  # alpha below this is export noise around the ink, not ink
LOGO_INK = 253  # alpha from this up is solid ink: full coverage
RING_HEIGHT = 115  # px: the room the boot page leaves for its mark at LOGO_WIDTH
# WOODLAND's faces, pixel fonts from the Soft Type collection, each drawn at
# the size where one of its pixels is one of the screen's (units per em over
# its pixel), 1 bit per pixel: Jersey for text, standing in for the menu's
# sizes and keeping their line heights, so every page keeps its layout; and
# titles in Jacquard 12, a pixel blackletter. The number in each name is the
# height of its capitals.
JERSEY = ROOT / "third_party/jersey"
WOODLAND_FACES = (  # the face, its pixel size and the menu sizes it stands in for
    (JERSEY / "Jersey10-Regular.ttf", 1400 / 75, (12, 16)),
    (JERSEY / "Jersey15-Regular.ttf", 1350 / 50, (20,)),
    (JERSEY / "Jersey20-Regular.ttf", 1292 / 38, (28,)),
)
BLACKLETTER = ROOT / "third_party/jacquard-12/Jacquard12-Regular.ttf"
BLACKLETTER_SIZE = 21  # 1260 / 60
PIXEL_SUPERSAMPLE = 8


def glyph(font: ImageFont.FreeTypeFont, size: int, character: str, baseline: int):
    """4-bit rows of one glyph and its LVGL descriptor fields."""
    canvas = Image.new("L", (size * 3, baseline + size), 0)
    origin = size
    ImageDraw.Draw(canvas).text((origin, baseline), character, font=font, fill=255, anchor="ls")
    advance = round(font.getlength(character))
    box = canvas.getbbox()
    if box is None:
        return [], advance, 0, 0, 0, 0
    x0, y0, x1, y1 = box
    pixels = canvas.crop(box)
    width, height = x1 - x0, y1 - y0
    levels = [(value * 15 + 127) // 255 for value in pixels.tobytes()]
    # LVGL reads bottom-up offsets: rows above the baseline are positive.
    return levels, advance, width, height, x0 - origin, baseline - y1


def pack(levels, bpp=4):
    """The first pixel in the highest bits, one continuous stream per glyph, like lv_font_conv."""
    data = bytearray()
    per_byte = 8 // bpp
    for index in range(0, len(levels), per_byte):
        byte = 0
        for offset in range(per_byte):
            level = levels[index + offset] if index + offset < len(levels) else 0
            byte |= level << (8 - bpp * (offset + 1))
        data.append(byte)
    return data


def pixel_glyph(font: ImageFont.FreeTypeFont, size: int, character: str):
    """1-bit rows of one glyph of a pixel face and its LVGL descriptor fields.

    `font` is the face at PIXEL_SUPERSAMPLE times the size where its pixels
    are the screen's, `size` that size rounded up. A pixel is lit when most of
    its block is inked."""
    scale = PIXEL_SUPERSAMPLE
    origin, baseline = size * scale, 2 * size * scale
    canvas = Image.new("L", (4 * size * scale, 3 * size * scale), 0)
    ImageDraw.Draw(canvas).text((origin, baseline), character, font=font, fill=255, anchor="ls")
    small = canvas.resize((canvas.width // scale, canvas.height // scale), Image.Resampling.BOX)
    small = small.point(lambda value: 1 if value >= 128 else 0)
    advance = round(font.getlength(character) / scale)
    box = small.getbbox()
    if box is None:
        return [], advance, 0, 0, 0, 0
    x0, y0, x1, y1 = box
    return list(small.crop(box).tobytes()), advance, x1 - x0, y1 - y0, x0 - size, 2 * size - y1


def c_bytes(data, indent="    ", per_line=16):
    lines = []
    for index in range(0, len(data), per_line):
        lines.append(indent + ", ".join(f"0x{value:02x}" for value in data[index:index + per_line]) + ",")
    return "\n".join(lines)


def c_string(text: str, indent="    ") -> str:
    """A C string literal, one source line per text line."""
    lines = []
    for line in text.splitlines(keepends=True):
        escaped = line.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
        lines.append(f'{indent}"{escaped}"')
    return "\n".join(lines)


def font_source(ttf: Path) -> str:
    digest = hashlib.sha256(ttf.read_bytes()).hexdigest()
    license_text = (ROOT / "third_party/share-tech-mono/OFL.txt").read_text()
    notice = license_text.splitlines()[0]
    parts = [
        "/* SPDX-License-Identifier: OFL-1.1-RFN",
        " *",
        f" * {notice}",
        " *",
        " * Generated by firmware/tools/make_menu_assets.py: the ASCII glyphs of",
        f" * ShareTechMono-Regular.ttf (sha256 {digest[:16]}...) rasterized at",
        f" * {', '.join(str(size) for size in SIZES)} px, 4 bits per pixel. A Modified Version of the Font",
        " * Software, licensed under the SIL Open Font License 1.1",
        " * (firmware/third_party/share-tech-mono/OFL.txt), not under the firmware's",
        " * license, and not named with the Reserved Font Name. Do not edit. */",
        '#include "pet_menu_assets.h"',
        "",
        "/* The copyright notice and license travel with every copy of the firmware;",
        " * the menu's ABOUT page shows them. */",
        "const char pet_menu_font_license[] =",
        c_string(license_text) + ";",
        "",
    ]
    for size in SIZES:
        font = ImageFont.truetype(str(ttf), size)
        ascent, descent = font.getmetrics()
        glyphs = [glyph(font, size, chr(code), ascent + size // 2) for code in range(FIRST, LAST + 1)]
        parts += font_c(glyphs, 4, [(f"pet_menu_font_{size}", ascent + descent, descent)])
    return "\n".join(parts)


def font_c(glyphs, bpp: int, fonts) -> list[str]:
    """LVGL fonts of the ASCII glyphs FIRST to LAST, rendered at `bpp` bits:
    one for each (name, line height, base line) of `fonts`, sharing them."""
    name = fonts[0][0]
    bitmap = bytearray()
    descriptors = ["    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},"]
    for code, (levels, advance, width, height, ofs_x, ofs_y) in zip(range(FIRST, LAST + 1), glyphs):
        descriptors.append(
            f"    {{.bitmap_index = {len(bitmap)}, .adv_w = {advance * 16}, .box_w = {width}, "
            f".box_h = {height}, .ofs_x = {ofs_x}, .ofs_y = {ofs_y}}}, /* {chr(code)!r} */")
        bitmap += pack(levels, bpp)
    return [
        f"static LV_ATTRIBUTE_LARGE_CONST const uint8_t {name}_bitmap[] = {{",
        c_bytes(bitmap),
        "};",
        f"static const lv_font_fmt_txt_glyph_dsc_t {name}_glyphs[] = {{",
        *descriptors,
        "};",
        f"static const lv_font_fmt_txt_cmap_t {name}_cmaps[] = {{",
        f"    {{.range_start = {FIRST}, .range_length = {LAST - FIRST + 1}, .glyph_id_start = 1,",
        "     .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,",
        "     .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},",
        "};",
        f"static const lv_font_fmt_txt_dsc_t {name}_dsc = {{",
        f"    .glyph_bitmap = {name}_bitmap, .glyph_dsc = {name}_glyphs, .cmaps = {name}_cmaps,",
        f"    .kern_dsc = NULL, .kern_scale = 0, .cmap_num = 1, .bpp = {bpp}, .kern_classes = 0, .bitmap_format = 0,",
        "};",
    ] + [line for font, line_height, base_line in fonts for line in (
        f"const lv_font_t {font} = {{",
        "    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,",
        "    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,",
        f"    .line_height = {line_height},",
        f"    .base_line = {base_line},",
        "    .subpx = LV_FONT_SUBPX_NONE,",
        "    .underline_position = -2,",
        "    .underline_thickness = 1,",
        f"    .dsc = &{name}_dsc,",
        "};",
    )] + [""]


def woodland_font_source() -> str:
    """WOODLAND's faces: Jersey 10, 15 and 20 for text and Jacquard 12 for titles."""
    sources = [face for face, _, _ in WOODLAND_FACES] + [BLACKLETTER]
    notices = [path.read_text().splitlines()[0] for path in (JERSEY / "OFL.txt", BLACKLETTER.parent / "OFL.txt")]
    parts = [
        "/* SPDX-License-Identifier: OFL-1.1",
        " *",
        *[f" * {notice}" for notice in notices],
        " *",
        " * Generated by firmware/tools/make_menu_assets.py: the ASCII glyphs of",
        *[f" * {path.name} (sha256 {hashlib.sha256(path.read_bytes()).hexdigest()[:16]}...)," for path in sources],
        " * each at the size where one of its pixels is one of the screen's, 1 bit",
        " * per pixel. Modified Versions of the Font Software, licensed under the SIL",
        " * Open Font License 1.1 (firmware/third_party/jersey/OFL.txt and",
        " * firmware/third_party/jacquard-12/OFL.txt), not under the firmware's license.",
        " * The menu's ABOUT page shows these notices and the license. Do not edit. */",
        '#include "pet_menu_assets.h"',
        "",
    ]
    for face, size, stands_for in WOODLAND_FACES:
        font = ImageFont.truetype(str(face), size * PIXEL_SUPERSAMPLE)
        glyphs = [pixel_glyph(font, math.ceil(size), chr(code)) for code in range(FIRST, LAST + 1)]
        metrics = [ImageFont.truetype(str(FONT), menu).getmetrics() for menu in stands_for]
        parts += font_c(glyphs, 1, [(f"pet_woodland_font_{menu}", ascent + descent, descent)
                                    for menu, (ascent, descent) in zip(stands_for, metrics)])
    font = ImageFont.truetype(str(BLACKLETTER), BLACKLETTER_SIZE * PIXEL_SUPERSAMPLE)
    ascent, descent = ImageFont.truetype(str(BLACKLETTER), BLACKLETTER_SIZE).getmetrics()
    glyphs = [pixel_glyph(font, BLACKLETTER_SIZE, chr(code)) for code in range(FIRST, LAST + 1)]
    parts += font_c(glyphs, 1, [("pet_woodland_title", ascent + descent, descent)])
    return "\n".join(parts)


class Pen:
    """Strokes and shapes in icon pixels, drawn supersampled."""

    def __init__(self, width: int, height: int):
        self.size = (width, height)
        self.image = Image.new("L", (width * SUPERSAMPLE, height * SUPERSAMPLE), 0)
        self.draw = ImageDraw.Draw(self.image)

    def scale(self, *values):
        return [value * SUPERSAMPLE for value in values]

    def arc(self, cx, cy, radius, start, end, width):
        self.draw.arc(self.scale(cx - radius, cy - radius, cx + radius, cy + radius), start, end,
                      fill=255, width=round(width * SUPERSAMPLE))

    def disc(self, cx, cy, rx, ry=None):
        ry = rx if ry is None else ry
        self.draw.ellipse(self.scale(cx - rx, cy - ry, cx + rx, cy + ry), fill=255)

    def hole(self, cx, cy, rx, ry=None):
        ry = rx if ry is None else ry
        self.draw.ellipse(self.scale(cx - rx, cy - ry, cx + rx, cy + ry), fill=0)

    def line(self, points, width):
        scaled = [tuple(self.scale(x, y)) for x, y in points]
        self.draw.line(scaled, fill=255, width=round(width * SUPERSAMPLE), joint="curve")
        for x, y in scaled:  # round caps
            r = width * SUPERSAMPLE / 2
            self.draw.ellipse((x - r, y - r, x + r, y + r), fill=255)

    def polygon(self, points):
        self.draw.polygon([tuple(self.scale(x, y)) for x, y in points], fill=255)

    def rounded(self, x0, y0, x1, y1, radius):
        self.draw.rounded_rectangle(self.scale(x0, y0, x1, y1), radius=radius * SUPERSAMPLE, fill=255)

    def mask(self):
        return self.image.resize(self.size, Image.Resampling.BOX)


# The story screens' pixel art: 12x12, one bit per pixel ('#' is ink), drawn
# here for this firmware. The menu shows each two or three times larger,
# without smoothing.
PIXEL_ICONS = {
    "flame": (".....#......", ".....##.....", "....###.....", "....####..#.", "...#####.##.", "..#########.",
              "..####.####.", ".####...###.", ".###....####", ".###..#..###", "..##.###.##.", "...#######.."),
    "shield": ("############", "#....##....#", "#....##....#", "#.########.#", "#.########.#", "#....##....#",
               ".#...##...#.", ".#...##...#.", "..#..##..#..", "...#.##.#...", "....####....", ".....##....."),
    "star": (".....##.....", ".....##.....", "....####....", "....####....", "############", ".##########.",
             "..########..", "...######...", "...######...", "..###..###..", "..##....##..", ".##......##."),
    "heart": ("............", ".###....###.", "#####..#####", "############", "############", "############",
              ".##########.", "..########..", "...######...", "....####....", ".....##.....", "............"),
    "bolt": (".......####.", "......####..", ".....####...", "....####....", "...#######..", "..#######...",
             ".....###....", "....###.....", "...###......", "..###.......", "..##........", ".#.........."),
    "eye": ("............", "............", "....####....", "..##....##..", ".#...##...#.", "#...####...#",
            "#...####...#", ".#...##...#.", "..##....##..", "....####....", "............", "............"),
    "leaf": ("..........##", ".......#####", ".....######.", "....####.##.", "...####.###.", "..####.####.",
             "..###.####..", "..##.####...", "..#.####....", "...####.....", "..#.........", ".#.........."),
    "moon": ("....####....", "..####......", ".####.......", ".###........", "####........", "####........",
             "####........", "####........", ".###........", ".####.......", "..####......", "....####...."),
    "key": ("............", "............", "............", ".####.......", "##..##......", "#....#######",
            "#....#######", "##..##..#.##", ".####...#.##", "............", "............", "............"),
    # The rest of a story game's blessing icons; its storm is the bolt.
    "feather": ("..........##", "........####", ".......####.", "......####.#", ".....####.#.", "....####.#..",
                "...####.#...", "...###.#....", "..##..#.....", "..#.##......", ".#..........", "#..........."),
    "potion": ("....####....", ".....##.....", ".....##.....", "....#..#....", "...#....#...", "..#......#..",
               ".#........#.", ".##########.", ".##########.", ".##########.", "..########..", "...######..."),
    "sword": ("..........##", ".........###", "........###.", ".......###..", "......###...", ".....###....",
              ".#..###.....", "..####......", "...##.......", "..####......", ".##..#......", "##.........."),
}


def blessing_icons() -> dict[str, tuple[str, ...]]:
    """Public pixel definitions for the additional story-blessings-v2 icons."""
    return {'blessing_mask': ('............',
                       '............',
                       '#..........#',
                       '##........##',
                       '############',
                       '##...##...##',
                       '##...##...##',
                       '############',
                       '.####..####.',
                       '..##....##..',
                       '............',
                       '............'),
     'blessing_hand': ('....#.#.....',
                       '..#.#.#.#...',
                       '..#.#.#.#...',
                       '..#.#.#.#...',
                       '..#.#.#.#.#.',
                       '..#######.#.',
                       '..#########.',
                       '..########..',
                       '...#######..',
                       '...######...',
                       '....#####...',
                       '....#####...'),
     'blessing_bubble': ('............',
                         '.##########.',
                         '############',
                         '##..#..#..##',
                         '##..#..#..##',
                         '############',
                         '############',
                         '.##########.',
                         '..###.......',
                         '..##........',
                         '..#.........',
                         '............'),
     'blessing_skull': ('...######...',
                        '..########..',
                        '.##########.',
                        '.##########.',
                        '.#..####..#.',
                        '.#..####..#.',
                        '.##########.',
                        '..###..###..',
                        '...######...',
                        '...#.##.#...',
                        '...######...',
                        '............'),
     'blessing_crown': ('............',
                        '.....##.....',
                        '#....##....#',
                        '##..####..##',
                        '###.####.###',
                        '############',
                        '############',
                        '##.##..##.##',
                        '############',
                        '############',
                        '............',
                        '............'),
     'blessing_coin': ('...######...',
                       '.##########.',
                       '.###....###.',
                       '###..##..###',
                       '##..###...##',
                       '##...##...##',
                       '##...##...##',
                       '##...##...##',
                       '###.####.###',
                       '.###....###.',
                       '.##########.',
                       '...######...'),
     'blessing_ring': ('....####....',
                       '.....##.....',
                       '...######...',
                       '..##....##..',
                       '.##......##.',
                       '.#........#.',
                       '.#........#.',
                       '.##......##.',
                       '..##....##..',
                       '...######...',
                       '............',
                       '............'),
     'blessing_scroll': ('.##########.',
                         '##........##',
                         '.##########.',
                         '..#......#..',
                         '..#.####.#..',
                         '..#......#..',
                         '..#.####.#..',
                         '..#......#..',
                         '.##########.',
                         '##........##',
                         '.##########.',
                         '............'),
     'blessing_bell': ('.....##.....',
                       '....####....',
                       '...######...',
                       '..########..',
                       '..########..',
                       '..########..',
                       '..########..',
                       '.##########.',
                       '############',
                       '............',
                       '.....##.....',
                       '............'),
     'blessing_hourglass': ('############',
                            '.##########.',
                            '..########..',
                            '...######...',
                            '....####....',
                            '.....##.....',
                            '.....##.....',
                            '....#..#....',
                            '...#.##.#...',
                            '..#.####.#..',
                            '.##########.',
                            '############'),
     'blessing_sun': ('.....##.....',
                      '.#...##...#.',
                      '..#......#..',
                      '....####....',
                      '...######...',
                      '##.######.##',
                      '##.######.##',
                      '...######...',
                      '....####....',
                      '..#......#..',
                      '.#...##...#.',
                      '.....##.....'),
     'blessing_cloud': ('............',
                        '............',
                        '.....###....',
                        '....#####...',
                        '.##.######..',
                        '############',
                        '############',
                        '############',
                        '.##########.',
                        '............',
                        '............',
                        '............'),
     'blessing_drop': ('.....##.....',
                       '.....##.....',
                       '....####....',
                       '....####....',
                       '...######...',
                       '..########..',
                       '..########..',
                       '.##.#######.',
                       '.##.#######.',
                       '.###.######.',
                       '..########..',
                       '...######...'),
     'blessing_pick': ('...######...',
                       '.###....###.',
                       '##...##...##',
                       '#....##....#',
                       '.....##.....',
                       '.....##.....',
                       '.....##.....',
                       '.....##.....',
                       '.....##.....',
                       '.....##.....',
                       '.....##.....',
                       '.....##.....'),
     'blessing_bow': ('.##.........',
                      '.#.#........',
                      '.#..#.......',
                      '.#...#....#.',
                      '.#....#...##',
                      '############',
                      '.#....#...##',
                      '.#...#....#.',
                      '.#..#.......',
                      '.#.#........',
                      '.##.........',
                      '............'),
     'blessing_lantern': ('....####....',
                          '....#..#....',
                          '...######...',
                          '..#......#..',
                          '..#.####.#..',
                          '..#.####.#..',
                          '..#.####.#..',
                          '..#.####.#..',
                          '..#......#..',
                          '...######...',
                          '....####....',
                          '............'),
     'blessing_gem': ('............',
                      '............',
                      '..########..',
                      '.##.####.##.',
                      '############',
                      '.##########.',
                      '..########..',
                      '...######...',
                      '....####....',
                      '.....##.....',
                      '............',
                      '............'),
     'blessing_ghost': ('...######...',
                        '..########..',
                        '.##########.',
                        '.##..##..##.',
                        '.##..##..##.',
                        '.##########.',
                        '.##########.',
                        '.##########.',
                        '.##########.',
                        '.##########.',
                        '.##########.',
                        '.#..##..#..#'),
     'blessing_paw': ('............',
                      '...##..##...',
                      '...##..##...',
                      '............',
                      '##........##',
                      '##..####..##',
                      '...######...',
                      '..########..',
                      '..########..',
                      '..########..',
                      '...##..##...',
                      '............'),
     'blessing_gear': ('.....##.....',
                       '..#.####.#..',
                       '.##########.',
                       '..###..###..',
                       '.###....###.',
                       '####....####',
                       '####....####',
                       '.###....###.',
                       '..###..###..',
                       '.##########.',
                       '..#.####.#..',
                       '.....##.....')}


def pixel_icon(rows) -> Image.Image:
    """A 1-bit drawing as an alpha mask: ink is fully opaque."""
    mask = Image.new("L", (len(rows[0]), len(rows)), 0)
    mask.putdata([255 if cell == "#" else 0 for row in rows for cell in row])
    return mask


def arrow_head(pen: Pen, cx, cy, radius, angle):
    """A clockwise arrow tip at `angle` degrees on a circle."""
    a = math.radians(angle)
    px, py = cx + radius * math.cos(a), cy + radius * math.sin(a)
    tx, ty = -math.sin(a), math.cos(a)
    nx, ny = math.cos(a), math.sin(a)
    pen.polygon([(px + tx * 3.2, py + ty * 3.2), (px + nx * 3 - tx * 0.8, py + ny * 3 - ty * 0.8),
                 (px - nx * 3 - tx * 0.8, py - ny * 3 - ty * 0.8)])


def icons():
    drawn = {}
    pen = Pen(26, 20)  # Wi-Fi: three waves over a dot
    for radius in (16, 11, 6):
        pen.arc(13, 18, radius, 225, 315, 2.6)
    pen.disc(13, 17.2, 2.3)
    drawn["wifi"] = pen
    pen = Pen(26, 22)  # speaker and two sound waves
    pen.polygon([(2, 8), (7, 8), (13, 3), (13, 19), (7, 14), (2, 14)])
    pen.arc(13, 11, 5.5, -45, 45, 2)
    pen.arc(13, 11, 10, -45, 45, 2)
    drawn["speaker"] = pen
    pen = Pen(24, 24)  # sun
    pen.disc(12, 12, 4.6)
    for step in range(8):
        a = math.radians(step * 45)
        pen.line([(12 + 7.6 * math.cos(a), 12 + 7.6 * math.sin(a)),
                  (12 + 10.4 * math.cos(a), 12 + 10.4 * math.sin(a))], 2)
    drawn["sun"] = pen
    pen = Pen(18, 24)  # microphone
    pen.rounded(6, 2, 12, 14.5, 3)
    pen.arc(9, 11, 6.2, 0, 180, 1.8)
    pen.line([(9, 17.5), (9, 21)], 1.8)
    pen.line([(5.5, 21.5), (12.5, 21.5)], 1.8)
    drawn["mic"] = pen
    pen = Pen(22, 22)  # paw
    pen.disc(11, 15, 5.6, 4.6)
    for x, y in ((4.3, 9.2), (8.4, 5.1), (13.6, 5.1), (17.7, 9.2)):
        pen.disc(x, y, 2.4, 2.8)
    drawn["paw"] = pen
    pen = Pen(20, 20)  # sync: two chasing arrows
    pen.arc(10, 10, 7, 200, 330, 2)
    pen.arc(10, 10, 7, 20, 150, 2)
    arrow_head(pen, 10, 10, 7, 334)
    arrow_head(pen, 10, 10, 7, 154)
    drawn["sync"] = pen
    pen = Pen(20, 20)  # power
    pen.arc(10, 11, 7, -55, 235, 2)
    pen.line([(10, 2.5), (10, 9.5)], 2)
    drawn["power"] = pen
    pen = Pen(8, 14)
    pen.line([(2, 2), (6, 7), (2, 12)], 1.8)
    drawn["chevron_right"] = pen
    pen = Pen(22, 10)
    pen.line([(3, 8), (11, 2), (19, 8)], 2)
    drawn["chevron_up"] = pen
    # The story screens' handles: chevron_up turned to the swipe that closes each.
    pen = Pen(22, 10)
    pen.line([(3, 2), (11, 8), (19, 2)], 2)
    drawn["handle_down"] = pen
    pen = Pen(10, 22)
    pen.line([(8, 3), (2, 11), (8, 19)], 2)
    drawn["handle_left"] = pen
    pen = Pen(10, 22)
    pen.line([(2, 3), (8, 11), (2, 19)], 2)
    drawn["handle_right"] = pen
    pen = Pen(24, 22)  # STYLE: a painter's palette, its thumb hole and three wells
    pen.disc(12, 11, 11, 9.5)
    pen.hole(3.5, 17, 3.6)
    pen.hole(15.5, 15, 2.6)
    for x, y in ((6.5, 8), (11.5, 5.5), (17, 7.5)):
        pen.hole(x, y, 2.1)
    drawn["palette"] = pen
    masks = {name: pen.mask() for name, pen in drawn.items()}
    masks.update({name: pixel_icon(rows) for name, rows in {**PIXEL_ICONS, **blessing_icons()}.items()})
    return masks


# WOODLAND's dark planks: a tile of two boards 16 px high, each with its grain,
# a light lip under the seam above it, a dark seam at its foot and one butt
# joint, drawn here pixel by pixel. Cards are panelled with it.
WOOD_SIZE = (128, 32)
WOOD_BOARD = 16
WOOD_BASE = (74, 46, 23)
WOOD_GRAIN = ((62, 38, 19), (84, 53, 27))  # darker and lighter streaks
WOOD_SEAM = (30, 18, 9)
WOOD_LIP = (96, 62, 31)
WOOD_JOINTS = (88, 30)  # where each board's end meets the next
WOOD_KNOTS = ((20, 5), (70, 22))
KNOT = ((0, 0), (1, 0), (2, 0), (-1, 1), (0, 1), (1, 1), (2, 1), (3, 1), (0, 2), (1, 2), (2, 2))


def lcg(seed: int):
    """A fixed sequence of numbers, the same on every machine."""
    state = seed
    while True:
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        yield state >> 8


def wood_planks() -> Image.Image:
    width, height = WOOD_SIZE
    image = Image.new("RGB", WOOD_SIZE, WOOD_BASE)
    pixels = image.load()
    numbers = lcg(7)
    for board, joint in enumerate(WOOD_JOINTS):
        top = board * WOOD_BOARD
        for _ in range(16):  # streaks that wrap around the tile, each stepping down once
            y = top + 2 + next(numbers) % (WOOD_BOARD - 5)
            x = next(numbers) % width
            length = 10 + next(numbers) % 44
            color = WOOD_GRAIN[0] if next(numbers) % 3 else WOOD_GRAIN[1]
            bend = next(numbers) % length
            for i in range(length):
                pixels[(x + i) % width, y + (i > bend)] = color
        for x in range(width):
            pixels[x, top] = WOOD_LIP
            pixels[x, top + WOOD_BOARD - 1] = WOOD_SEAM
        for y in range(top, top + WOOD_BOARD - 1):
            pixels[joint, y] = WOOD_SEAM
            pixels[joint + 1, y] = WOOD_LIP
    for x, y in WOOD_KNOTS:
        for dx, dy in KNOT:
            pixels[x + dx, y + dy] = WOOD_GRAIN[0]
        pixels[x + 1, y + 1] = WOOD_SEAM
    return image


# A signboard's rope, twisted: a tile 2 px wide that repeats downward.
ROPE = ("LS", "LL", "SL", "SS")
ROPE_COLORS = {"L": (214, 178, 112), "S": (128, 92, 46)}


def wood_rope() -> Image.Image:
    image = Image.new("RGB", (len(ROPE[0]), len(ROPE)))
    image.putdata([ROPE_COLORS[cell] for row in ROPE for cell in row])
    return image


def rgb565_image(name: str, image: Image.Image) -> list[str]:
    """An RGB image as an LVGL RGB565 image, drawn as it is."""
    width, height = image.size
    rgb = image.tobytes()
    data = bytearray()
    for i in range(0, len(rgb), 3):
        red, green, blue = rgb[i:i + 3]
        data += ((red >> 3) << 11 | (green >> 2) << 5 | blue >> 3).to_bytes(2, "little")
    return [
        f"static const uint8_t {name}_map[] = {{",
        c_bytes(data),
        "};",
        f"const lv_image_dsc_t {name} = {{",
        "    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,",
        f"               .w = {width}, .h = {height}, .stride = {width * 2}}},",
        f"    .data_size = sizeof({name}_map),",
        f"    .data = {name}_map,",
        "};",
        "",
    ]


def logo(path: Path) -> Image.Image:
    """A PNG's alpha as an 8-bit coverage mask, LOGO_WIDTH px wide.

    The alpha is the ink: specks fainter than LOGO_NOISE are dropped and alpha
    from LOGO_INK up counts as full coverage. The ink's box is scaled by one
    factor in both directions, each pixel the average of the source pixels it
    covers, so the geometry and proportions stay the file's own. The part of a
    pixel row the ink does not fill is split above and below it."""
    alpha = Image.open(path).getchannel("A")
    alpha = alpha.point(lambda value: 0 if value < LOGO_NOISE else min(255, round(value * 255 / LOGO_INK)))
    x0, y0, x1, y1 = alpha.getbbox()
    scale = LOGO_WIDTH / (x1 - x0)
    height = math.ceil((y1 - y0) * scale)
    spare = height / scale - (y1 - y0)
    return alpha.resize((LOGO_WIDTH, height), Image.Resampling.BOX,
                        box=(x0, y0 - spare / 2, x1, y1 + spare / 2))


def ring() -> Image.Image:
    """The boot mark without a logo: a plain ring, LOGO_WIDTH by RING_HEIGHT px."""
    pen = Pen(LOGO_WIDTH, RING_HEIGHT)
    pen.arc(LOGO_WIDTH / 2, RING_HEIGHT / 2, 48, 0, 360, 7)
    return pen.mask()


def mask_image(name: str, mask: Image.Image) -> list[str]:
    """An 8-bit alpha mask as an LVGL A8 image the menu tints."""
    width, height = mask.size
    return [
        f"static const uint8_t {name}_map[] = {{",
        c_bytes(mask.tobytes()),
        "};",
        f"const lv_image_dsc_t {name} = {{",
        "    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8,",
        f"               .w = {width}, .h = {height}, .stride = {width}}},",
        f"    .data_size = sizeof({name}_map),",
        f"    .data = {name}_map,",
        "};",
        "",
    ]


def icon_source(logo_path: Path | None = None) -> str:
    parts = [
        "/* Generated by firmware/tools/make_menu_assets.py. Do not edit. */",
        '#include "pet_menu_assets.h"',
        "",
    ]
    for name, mask in icons().items():
        parts += mask_image(f"pet_icon_{name}", mask)
    parts.append("/* WOODLAND's plank tile, which its cards are panelled with, and its rope. */")
    parts += rgb565_image("pet_wood_planks", wood_planks())
    parts += rgb565_image("pet_wood_rope", wood_rope())
    if logo_path is None:
        parts.append("/* The boot page's mark, a plain ring: the coverage mask the page tints lime. */")
        parts += mask_image("pet_boot_mark", ring())
    else:
        digest = hashlib.sha256(logo_path.read_bytes()).hexdigest()
        parts.append(f"/* The boot page's mark, the coverage mask the page tints lime: made from {logo_path.name}")
        parts.append(f" * (sha256 {digest}). */")
        parts += mask_image("pet_boot_mark", logo(logo_path))
    return "\n".join(parts)


def header_source() -> str:
    lines = [
        "/* The menu's faces (pet_menu_font.c, SIL OFL 1.1), icons and boot mark",
        " * (pet_menu_icons.c), generated by firmware/tools/make_menu_assets.py. */",
        "#pragma once",
        "",
        '#include "lvgl.h"',
        "",
        *[f"extern const lv_font_t pet_menu_font_{size};" for size in SIZES],
        "/* The fonts' copyright notice and SIL OFL 1.1 text, verbatim. */",
        "extern const char pet_menu_font_license[];",
        "/* WOODLAND's faces (pet_menu_woodland_font.c, SIL OFL 1.1): Jersey at each of",
        " * the menu's sizes, and Jacquard 12 for titles. */",
        *[f"extern const lv_font_t pet_woodland_font_{size};" for size in SIZES],
        "extern const lv_font_t pet_woodland_title;",
        "",
        *[f"extern const lv_image_dsc_t pet_icon_{name};" for name in icons()],
        "/* WOODLAND's plank and rope tiles, RGB565. */",
        "extern const lv_image_dsc_t pet_wood_planks;",
        "extern const lv_image_dsc_t pet_wood_rope;",
        "/* The boot page's mark, an 8-bit mask the page tints. */",
        "extern const lv_image_dsc_t pet_boot_mark;",
        "",
    ]
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output", type=Path, default=ROOT / "main")
    parser.add_argument("--logo", type=Path, help="PNG whose alpha becomes the boot mark (default: a plain ring)")
    arguments = parser.parse_args()
    arguments.output.mkdir(parents=True, exist_ok=True)
    (arguments.output / "pet_menu_font.c").write_text(font_source(FONT))
    (arguments.output / "pet_menu_woodland_font.c").write_text(woodland_font_source())
    (arguments.output / "pet_menu_icons.c").write_text(icon_source(arguments.logo))
    (arguments.output / "pet_menu_assets.h").write_text(header_source())
    for name in ("pet_menu_font.c", "pet_menu_woodland_font.c", "pet_menu_icons.c", "pet_menu_assets.h"):
        print(arguments.output / name)


if __name__ == "__main__":
    main()
