"""Render existing logo assets at native pixel sizes for visual inspection."""

from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "out" / "branding-review.png"
SIZES = (16, 24, 32, 48, 64, 128, 256)


def main() -> None:
    sheet = Image.new("RGB", (1120, 820), "#eeeeee")
    draw = ImageDraw.Draw(sheet)
    draw.text((16, 12), "Windows icon frames at native pixel size (view at 100%)", fill="black")
    with Image.open(ROOT / "resources/branding/app-icon.ico") as icon:
        for row, background in enumerate(("#ffffff", "#101114")):
            y = 42 + row * 310
            draw.rectangle((10, y, 1110, y + 300), fill=background)
            x = 24
            for size in SIZES:
                frame = icon.ico.getimage((size, size)).convert("RGBA")
                draw.text((x, y + 8), str(size), fill="#888888")
                sheet.paste(frame, (x, y + 34), frame)
                x += max(size + 28, 100)
        draw.text((16, 680), "16 / 24 / 32 px enlarged 3x for pixel inspection (not display size)", fill="black")
        for index, size in enumerate((16, 24, 32)):
            frame = icon.ico.getimage((size, size)).convert("RGBA")
            frame = frame.resize((size * 3, size * 3), Image.Resampling.NEAREST)
            sheet.paste(frame, (16 + index * 140, 706), frame)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(OUTPUT)
    print(OUTPUT)


if __name__ == "__main__":
    main()
