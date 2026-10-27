"""Regenerate PNG and Windows icon sizes from the single editable branding SVG."""

from pathlib import Path
import subprocess

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "resources" / "branding" / "app-logo.png"
DESTINATION = ROOT / "resources" / "branding" / "app-icon.ico"
SIZES = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]


def main() -> None:
    subprocess.run(["node", str(ROOT / "tools/render-brand-svg.cjs")], check=True)
    with Image.open(SOURCE) as source:
        artwork = source.convert("RGBA")
        # Windows icon frames are square; pad wide or tall artwork without stretching it.
        edge = max(artwork.size)
        canvas = Image.new("RGBA", (edge, edge), (0, 0, 0, 0))
        canvas.paste(artwork, ((edge - artwork.width) // 2, (edge - artwork.height) // 2))
        canvas.save(DESTINATION, format="ICO", sizes=SIZES)
    print(f"Updated {DESTINATION} from {SOURCE}")


if __name__ == "__main__":
    main()
